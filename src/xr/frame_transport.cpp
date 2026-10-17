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
    FrameMeta meta;
};

// One ring of shared textures per channel (0 = left eye / mono / theater, 1 = right
// eye), so the two images of a both-eyes frame don't overwrite each other. The
// fences are shared: values only grow on each side's single context.
struct Channel {
    // Shared between the threads, guarded by g_mutex.
    SlotState slots[k_slots];
    uint64_t generation = 0; // bumped when the slot textures are recreated
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int newest = -1;
    int claimed = -1;
    uint64_t sequence = 0; // bumped per published frame

    // Game side only.
    ID3D11Texture2D* game_textures[k_slots]{};
    D3D11_TEXTURE2D_DESC game_desc{};

    // XR side only.
    uint64_t xr_generation = 0;
    uint64_t xr_last_sequence = 0;
    ID3D11Texture2D* xr_textures[k_slots]{};
};

std::mutex g_mutex;
Channel g_channels[k_channels];
HANDLE g_game_fence_handle = nullptr;
HANDLE g_xr_fence_handle = nullptr;
std::atomic<bool> g_fenced{false};
std::atomic<const char*> g_description{"-"};

// Game side only (the game's immediate context: Present, or the draw job while the
// main thread waits for it).
struct GameSide {
    bool initialized = false;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* game_fence = nullptr;
    ID3D11Fence* xr_fence = nullptr;
    uint64_t game_value = 0;
} g_game;

// XR thread only.
struct XrSide {
    bool fences_opened = false;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* game_fence = nullptr;
    ID3D11Fence* xr_fence = nullptr;
    uint64_t xr_value = 0;
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

bool ensure_game_textures(Channel& channel, ID3D11Device* device, const D3D11_TEXTURE2D_DESC& backbuffer) {
    if (channel.game_textures[0] != nullptr && channel.game_desc.Width == backbuffer.Width &&
        channel.game_desc.Height == backbuffer.Height && channel.game_desc.Format == backbuffer.Format) {
        return true;
    }

    for (auto& texture : channel.game_textures) {
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
            SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &channel.game_textures[i])) &&
            SUCCEEDED(channel.game_textures[i]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) &&
            SUCCEEDED(resource->GetSharedHandle(&handles[i])) && handles[i] != nullptr;
        release_com(resource);
        if (!ok) {
            logger::error("[transport] could not create shared texture %d", i);
            for (auto& texture : channel.game_textures) {
                release_com(texture);
            }
            return false;
        }
    }
    channel.game_desc = desc;

    std::scoped_lock _{g_mutex};
    for (int i = 0; i < k_slots; ++i) {
        channel.slots[i] = {handles[i], 0, 0, {}};
    }
    channel.width = desc.Width;
    channel.height = desc.Height;
    channel.format = desc.Format;
    channel.newest = -1;
    channel.claimed = -1;
    ++channel.generation;
    logger::info("[transport] channel %d: %d shared textures %ux%u format %d (generation %llu)",
                 int(&channel - g_channels), k_slots, desc.Width, desc.Height, (int)desc.Format,
                 (unsigned long long)channel.generation);
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

bool publish(int channel_index, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
             const FrameMeta& meta) {
    if (!g_game.initialized) {
        init_game_side(device, context);
    }

    auto& channel = g_channels[channel_index];
    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (!ensure_game_textures(channel, device, desc)) {
        return false;
    }

    int slot = -1;
    uint64_t read_done = 0;
    {
        std::scoped_lock _{g_mutex};
        for (int i = 0; i < k_slots && slot < 0; ++i) {
            if (i != channel.newest && i != channel.claimed) {
                slot = i;
            }
        }
        read_done = channel.slots[slot].xr_value;
    }

    const bool fenced = g_fenced;
    if (fenced && read_done > 0) {
        g_game.context4->Wait(g_game.xr_fence, read_done); // GPU-side: the XR copy out of this slot is done
    }

    if (desc.SampleDesc.Count > 1) {
        context->ResolveSubresource(channel.game_textures[slot], 0, backbuffer, 0, desc.Format);
    } else {
        context->CopyResource(channel.game_textures[slot], backbuffer);
    }

    if (fenced) {
        g_game.context4->Signal(g_game.game_fence, ++g_game.game_value);
    } else {
        context->Flush();
    }

    std::scoped_lock _{g_mutex};
    channel.slots[slot].game_value = g_game.game_value;
    channel.slots[slot].meta = meta;
    channel.newest = slot;
    ++channel.sequence;
    return true;
}

std::optional<Frame> claim_newest(int channel_index, ID3D11Device* device) {
    auto& channel = g_channels[channel_index];
    Frame frame;
    HANDLE handles[k_slots]{};
    {
        std::scoped_lock _{g_mutex};
        if (channel.newest < 0 || channel.sequence == channel.xr_last_sequence) {
            return std::nullopt;
        }
        frame = {channel.newest, channel.generation, channel.slots[channel.newest].game_value, channel.width,
                 channel.height, channel.format, channel.slots[channel.newest].meta, channel_index};
        for (int i = 0; i < k_slots; ++i) {
            handles[i] = channel.slots[i].handle;
        }
        channel.claimed = channel.newest;
        channel.xr_last_sequence = channel.sequence;
    }

    if (g_fenced && !g_xr.fences_opened && !open_xr_fences(device)) {
        g_fenced = false; // the game side reads this before its next GPU wait
        g_description = "shared textures, flush only (fence open failed)";
    }

    if (frame.generation != channel.xr_generation) {
        for (auto& texture : channel.xr_textures) {
            release_com(texture);
        }
        for (int i = 0; i < k_slots; ++i) {
            if (FAILED(device->OpenSharedResource(handles[i], __uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&channel.xr_textures[i])))) {
                logger::error("[transport] could not open shared texture %d on the XR device", i);
                release(frame);
                return std::nullopt;
            }
        }
        channel.xr_generation = frame.generation;
    }

    return frame;
}

void copy_and_release(const Frame& frame, ID3D11DeviceContext* context, ID3D11Texture2D* destination) {
    const bool fenced = g_fenced && g_xr.context4 != nullptr;
    if (fenced) {
        g_xr.context4->Wait(g_xr.game_fence, frame.game_value); // GPU-side: the game's write is done
    }

    auto& channel = g_channels[frame.channel];
    context->CopyResource(destination, channel.xr_textures[frame.slot]);

    if (fenced) {
        g_xr.context4->Signal(g_xr.xr_fence, ++g_xr.xr_value);
    }

    std::scoped_lock _{g_mutex};
    if (frame.generation == channel.generation) {
        channel.slots[frame.slot].xr_value = fenced ? g_xr.xr_value : 0;
        if (channel.claimed == frame.slot) {
            channel.claimed = -1;
        }
    }
}

void release(const Frame& frame) {
    auto& channel = g_channels[frame.channel];
    std::scoped_lock _{g_mutex};
    if (frame.generation == channel.generation && channel.claimed == frame.slot) {
        channel.claimed = -1;
    }
}

const char* description() {
    return g_description;
}
} // namespace xr::transport
