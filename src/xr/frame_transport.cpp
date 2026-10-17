#include "xr/frame_transport.hpp"

#include <atomic>
#include <mutex>

#include <d3d11_4.h>

#include "log.hpp"

namespace xr::transport {
namespace {
constexpr int k_slots = 3;

template <typename T>
void release_com(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

struct SlotState {
    HANDLE handle = nullptr;
    uint64_t game_value = 0; // game_fence value once the game's write is done
    uint64_t xr_value = 0;   // xr_fence value once the XR side's last read is done
};

// Shared between the threads, guarded by g_mutex.
std::mutex g_mutex;
SlotState g_slots[k_slots];
uint64_t g_generation = 0; // bumped when the slot textures are recreated
uint32_t g_width = 0, g_height = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
int g_newest = -1;
int g_claimed = -1;
uint64_t g_sequence = 0; // bumped per published frame
HANDLE g_game_fence_handle = nullptr;
HANDLE g_xr_fence_handle = nullptr;
std::atomic<bool> g_fenced{false};
std::atomic<const char*> g_description{"-"};

// Render thread only.
struct GameSide {
    bool initialized = false;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* game_fence = nullptr;
    ID3D11Fence* xr_fence = nullptr;
    uint64_t game_value = 0;
    ID3D11Texture2D* textures[k_slots]{};
    D3D11_TEXTURE2D_DESC desc{};
} g_game;

// XR thread only.
struct XrSide {
    bool fences_opened = false;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* game_fence = nullptr;
    ID3D11Fence* xr_fence = nullptr;
    uint64_t xr_value = 0;
    uint64_t generation = 0;
    uint64_t last_sequence = 0;
    ID3D11Texture2D* textures[k_slots]{};
} g_xr;

void init_game_side(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_game.initialized = true;

    ID3D11Device5* device5 = nullptr;
    HANDLE game_handle = nullptr, xr_handle = nullptr;
    const bool ok =
        SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) &&
        SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_game.context4))) &&
        SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void**>(&g_game.game_fence))) &&
        SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void**>(&g_game.xr_fence))) &&
        SUCCEEDED(g_game.game_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &game_handle)) &&
        SUCCEEDED(g_game.xr_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &xr_handle));
    release_com(device5);

    if (!ok) {
        release_com(g_game.context4);
        release_com(g_game.game_fence);
        release_com(g_game.xr_fence);
        g_description = "shared textures, flush only (no shared fences)";
        logger::warn("[transport] shared D3D11 fences unavailable; falling back to flushes");
        return;
    }

    std::scoped_lock _{g_mutex};
    g_game_fence_handle = game_handle;
    g_xr_fence_handle = xr_handle;
    g_fenced = true;
    g_description = "shared textures + fences";
    logger::info("[transport] shared fences created");
}

bool ensure_game_textures(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backbuffer) {
    if (g_game.textures[0] != nullptr && g_game.desc.Width == backbuffer.Width &&
        g_game.desc.Height == backbuffer.Height && g_game.desc.Format == backbuffer.Format) {
        return true;
    }

    for (auto& texture : g_game.textures) {
        release_com(texture);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = backbuffer.Width;
    desc.Height = backbuffer.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = backbuffer.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HANDLE handles[k_slots]{};
    for (int i = 0; i < k_slots; ++i) {
        IDXGIResource* resource = nullptr;
        const bool ok =
            SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &g_game.textures[i])) &&
            SUCCEEDED(g_game.textures[i]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) &&
            SUCCEEDED(resource->GetSharedHandle(&handles[i])) && handles[i] != nullptr;
        release_com(resource);
        if (!ok) {
            logger::error("[transport] could not create shared texture %d", i);
            for (auto& texture : g_game.textures) {
                release_com(texture);
            }
            return false;
        }
    }
    g_game.desc = desc;

    std::scoped_lock _{g_mutex};
    for (int i = 0; i < k_slots; ++i) {
        g_slots[i] = {handles[i], 0, 0};
    }
    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;
    g_newest = -1;
    g_claimed = -1;
    ++g_generation;
    logger::info("[transport] %d shared textures %ux%u format %d (generation %llu)", k_slots, desc.Width,
                 desc.Height, (int)desc.Format, (unsigned long long)g_generation);
    return true;
}

bool open_xr_fences(ID3D11Device* device) {
    g_xr.fences_opened = true;

    HANDLE game_handle, xr_handle;
    {
        std::scoped_lock _{g_mutex};
        game_handle = g_game_fence_handle;
        xr_handle = g_xr_fence_handle;
    }

    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const bool ok =
        SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5))) &&
        SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_xr.context4))) &&
        SUCCEEDED(device5->OpenSharedFence(game_handle, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_xr.game_fence))) &&
        SUCCEEDED(device5->OpenSharedFence(xr_handle, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_xr.xr_fence)));
    release_com(device5);
    release_com(context);

    if (!ok) {
        logger::error("[transport] could not open the shared fences on the XR device");
    }
    return ok;
}
} // namespace

bool publish(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer) {
    if (!g_game.initialized) {
        init_game_side(device, context);
    }

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (!ensure_game_textures(device, desc)) {
        return false;
    }

    int slot = -1;
    uint64_t read_done = 0;
    {
        std::scoped_lock _{g_mutex};
        for (int i = 0; i < k_slots && slot < 0; ++i) {
            if (i != g_newest && i != g_claimed) {
                slot = i;
            }
        }
        read_done = g_slots[slot].xr_value;
    }

    const bool fenced = g_fenced;
    if (fenced && read_done > 0) {
        g_game.context4->Wait(g_game.xr_fence, read_done); // GPU-side: the XR copy out of this slot is done
    }

    if (desc.SampleDesc.Count > 1) {
        context->ResolveSubresource(g_game.textures[slot], 0, backbuffer, 0, desc.Format);
    } else {
        context->CopyResource(g_game.textures[slot], backbuffer);
    }

    if (fenced) {
        g_game.context4->Signal(g_game.game_fence, ++g_game.game_value);
    } else {
        context->Flush();
    }

    std::scoped_lock _{g_mutex};
    g_slots[slot].game_value = g_game.game_value;
    g_newest = slot;
    ++g_sequence;
    return true;
}

std::optional<Frame> claim_newest(ID3D11Device* device) {
    Frame frame;
    HANDLE handles[k_slots]{};
    {
        std::scoped_lock _{g_mutex};
        if (g_newest < 0 || g_sequence == g_xr.last_sequence) {
            return std::nullopt;
        }
        frame = {g_newest, g_generation, g_slots[g_newest].game_value, g_width, g_height, g_format};
        for (int i = 0; i < k_slots; ++i) {
            handles[i] = g_slots[i].handle;
        }
        g_claimed = g_newest;
        g_xr.last_sequence = g_sequence;
    }

    if (g_fenced && !g_xr.fences_opened && !open_xr_fences(device)) {
        g_fenced = false; // the game side reads this before its next GPU wait
        g_description = "shared textures, flush only (fence open failed)";
    }

    if (frame.generation != g_xr.generation) {
        for (auto& texture : g_xr.textures) {
            release_com(texture);
        }
        for (int i = 0; i < k_slots; ++i) {
            if (FAILED(device->OpenSharedResource(handles[i], __uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&g_xr.textures[i])))) {
                logger::error("[transport] could not open shared texture %d on the XR device", i);
                release(frame);
                return std::nullopt;
            }
        }
        g_xr.generation = frame.generation;
    }

    return frame;
}

void copy_and_release(const Frame& frame, ID3D11DeviceContext* context, ID3D11Texture2D* destination) {
    const bool fenced = g_fenced && g_xr.context4 != nullptr;
    if (fenced) {
        g_xr.context4->Wait(g_xr.game_fence, frame.game_value); // GPU-side: the game's write is done
    }

    context->CopyResource(destination, g_xr.textures[frame.slot]);

    if (fenced) {
        g_xr.context4->Signal(g_xr.xr_fence, ++g_xr.xr_value);
    }

    std::scoped_lock _{g_mutex};
    if (frame.generation == g_generation) {
        g_slots[frame.slot].xr_value = fenced ? g_xr.xr_value : 0;
        if (g_claimed == frame.slot) {
            g_claimed = -1;
        }
    }
}

void release(const Frame& frame) {
    std::scoped_lock _{g_mutex};
    if (frame.generation == g_generation && g_claimed == frame.slot) {
        g_claimed = -1;
    }
}

const char* description() {
    return g_description;
}
} // namespace xr::transport
