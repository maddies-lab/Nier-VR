#include "game/effect_hunt.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include <windows.h>

#include "log.hpp"

namespace game::effect_hunt {
namespace {
constexpr UINT k_max_vertices = 6;
constexpr UINT k_min_width = 1280;

struct Candidate {
    uintptr_t ps = 0;
    uint32_t draws = 0;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool blend = false;
    D3D11_BLEND src = D3D11_BLEND_ONE, dst = D3D11_BLEND_ZERO;
    UINT ps_b1 = 0; // size of the PS constant buffer at b1, 0 if none
    std::string textures;
};

std::mutex g_mutex;
std::vector<Candidate> g_candidates;
std::atomic<int> g_state{0}; // 0 idle, 1 armed (starts at next Present), 2 recording
std::atomic<uintptr_t> g_disabled_ps{0};
int g_index = -1;
bool g_end_down = false, g_next_down = false, g_previous_down = false;

void record(ID3D11DeviceContext* context, uintptr_t ps, const D3D11_TEXTURE2D_DESC& target) {
    std::scoped_lock _{g_mutex};
    for (auto& c : g_candidates) {
        if (c.ps == ps) {
            ++c.draws;
            return;
        }
    }
    if (g_candidates.size() >= 64) {
        return;
    }
    Candidate c;
    c.ps = ps;
    c.draws = 1;
    c.width = target.Width;
    c.height = target.Height;
    c.format = target.Format;

    ID3D11BlendState* blend = nullptr;
    float factor[4];
    UINT mask = 0;
    context->OMGetBlendState(&blend, factor, &mask);
    if (blend != nullptr) {
        D3D11_BLEND_DESC desc{};
        blend->GetDesc(&desc);
        c.blend = desc.RenderTarget[0].BlendEnable != FALSE;
        c.src = desc.RenderTarget[0].SrcBlend;
        c.dst = desc.RenderTarget[0].DestBlend;
        blend->Release();
    }

    ID3D11Buffer* cb = nullptr;
    context->PSGetConstantBuffers(1, 1, &cb);
    if (cb != nullptr) {
        D3D11_BUFFER_DESC desc{};
        cb->GetDesc(&desc);
        c.ps_b1 = desc.ByteWidth;
        cb->Release();
    }

    ID3D11ShaderResourceView* srvs[6]{};
    context->PSGetShaderResources(0, 6, srvs);
    for (int i = 0; i < 6; ++i) {
        char entry[40] = " -";
        if (srvs[i] != nullptr) {
            ID3D11Resource* resource = nullptr;
            srvs[i]->GetResource(&resource);
            D3D11_RESOURCE_DIMENSION dimension{};
            if (resource != nullptr) {
                resource->GetType(&dimension);
            }
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC desc{};
                static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
                std::snprintf(entry, sizeof(entry), " t%d:%ux%u/f%d", i, desc.Width, desc.Height, (int)desc.Format);
            } else {
                std::snprintf(entry, sizeof(entry), " t%d:other", i);
            }
            if (resource != nullptr) {
                resource->Release();
            }
            srvs[i]->Release();
        }
        c.textures += entry;
    }
    g_candidates.push_back(c);
}

void report() {
    std::scoped_lock _{g_mutex};
    logger::info("[hunt] %zu full-screen passes in one frame (Page Down / Page Up switches one off):",
                 g_candidates.size());
    for (size_t i = 0; i < g_candidates.size(); ++i) {
        const auto& c = g_candidates[i];
        logger::info("[hunt] #%zu ps %llx x%u -> %ux%u f%d, blend %s (src %d dst %d), PS b1 %u B, textures%s", i,
                     (unsigned long long)c.ps, c.draws, c.width, c.height, (int)c.format, c.blend ? "on" : "off",
                     (int)c.src, (int)c.dst, c.ps_b1, c.textures.c_str());
    }
}

void select(int index) {
    std::scoped_lock _{g_mutex};
    const int count = static_cast<int>(g_candidates.size());
    if (count == 0) {
        return;
    }
    if (index < -1) {
        index = count - 1;
    }
    if (index >= count) {
        index = -1;
    }
    g_index = index;
    g_disabled_ps = index < 0 ? 0 : g_candidates[index].ps;
    if (index < 0) {
        logger::info("[hunt] nothing switched off");
    } else {
        logger::info("[hunt] #%d (ps %llx) switched off", index, (unsigned long long)g_candidates[index].ps);
    }
}

bool pressed(int key, bool& was_down) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}
} // namespace

bool should_skip(ID3D11DeviceContext* context, UINT count) {
    const int state = g_state;
    const uintptr_t disabled = g_disabled_ps;
    if ((state != 2 && disabled == 0) || count > k_max_vertices) {
        return false;
    }

    ID3D11PixelShader* ps = nullptr;
    context->PSGetShader(&ps, nullptr, nullptr);
    const auto key = reinterpret_cast<uintptr_t>(ps);
    if (ps != nullptr) {
        ps->Release();
    }
    if (key == 0) {
        return false;
    }
    if (state != 2) {
        return key == disabled;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv == nullptr) {
        return key == disabled;
    }
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if (resource != nullptr) {
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC target{};
            static_cast<ID3D11Texture2D*>(resource)->GetDesc(&target);
            if (target.Width >= k_min_width) {
                record(context, key, target);
            }
        }
        resource->Release();
    }
    return key == disabled;
}

void on_present() {
    if (g_state == 2) {
        g_state = 0;
        report();
    } else if (g_state == 1) {
        g_state = 2;
    }

    if (pressed(VK_END, g_end_down)) {
        std::scoped_lock _{g_mutex};
        g_candidates.clear();
        g_index = -1;
        g_disabled_ps = 0;
        g_state = 1;
        logger::info("[hunt] End: recording the next frame's full-screen passes");
    }
    if (pressed(VK_NEXT, g_next_down)) {
        select(g_index + 1);
    }
    if (pressed(VK_PRIOR, g_previous_down)) {
        select(g_index - 1);
    }
}

std::string status() {
    std::scoped_lock _{g_mutex};
    if (g_candidates.empty()) {
        return "";
    }
    char line[96];
    if (g_index < 0) {
        std::snprintf(line, sizeof(line), "effect hunt: %zu passes, none off (PgDn/PgUp)", g_candidates.size());
    } else {
        std::snprintf(line, sizeof(line), "effect hunt: #%d of %zu OFF (ps %llx)", g_index, g_candidates.size(),
                      (unsigned long long)g_candidates[g_index].ps);
    }
    return line;
}
} // namespace game::effect_hunt
