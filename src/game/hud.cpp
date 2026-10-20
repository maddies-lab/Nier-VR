#include "game/hud.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <tuple>
#include <vector>

#include <windows.h>

#include "hooks/context_hook.hpp"
#include "log.hpp"

namespace game::hud {
namespace {
constexpr UINT k_scene_buffer_size = 256;
constexpr UINT k_min_target_width = 1280;

std::atomic<Mode> g_mode{Mode::off};
std::atomic<bool> g_enabled{true};
bool g_key_was_down = false;

// 256-byte constant buffers last uploaded with an orthographic projection
// (SceneBuffer +0x40, transposed: last row 0 0 0 1).
std::mutex g_ortho_mutex;
std::vector<ID3D11Resource*> g_ortho;
thread_local ID3D11Resource* t_mapped = nullptr;
thread_local const float* t_mapped_data = nullptr;

// Draw-job thread.
ID3D11Texture2D* g_texture = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
D3D11_TEXTURE2D_DESC g_desc{};
bool g_captured_any = false;
bool g_cleared = false;
bool g_logged_first = false;
std::vector<std::tuple<uintptr_t, UINT, UINT>> g_logged_kinds;
std::vector<uintptr_t> g_sprite_ps;    // pixel shaders that have drawn HUD sprites
UINT g_full_width = 0;                 // widest R11G11B10 target seen (the scene target)

// State saved around a redirected draw.
bool g_redirected = false;
ID3D11RenderTargetView* g_saved_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
ID3D11DepthStencilView* g_saved_dsv = nullptr;
ID3D11BlendState* g_saved_blend = nullptr;
float g_saved_factor[4]{};
UINT g_saved_mask = 0;

struct BlendPair {
    ID3D11BlendState* original;
    ID3D11BlendState* hud;
    bool needs_factor;
};
std::vector<BlendPair> g_blends;

void on_map(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT hr) {
    t_mapped = nullptr;
    if (FAILED(hr) || type != D3D11_MAP_WRITE_DISCARD) {
        return;
    }
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (desc.ByteWidth == k_scene_buffer_size && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) {
        t_mapped = resource;
        t_mapped_data = static_cast<const float*>(mapped->pData);
    }
}

void on_unmap(ID3D11Resource* resource) {
    if (resource != t_mapped) {
        return;
    }
    t_mapped = nullptr;
    float projection[16];
    std::memcpy(projection, t_mapped_data + 16, sizeof(projection)); // one read of mapped memory
    const bool ortho = projection[0] != 0.0f && projection[12] == 0.0f && projection[13] == 0.0f &&
                       projection[14] == 0.0f && projection[15] == 1.0f;

    std::scoped_lock _{g_ortho_mutex};
    const auto it = std::find(g_ortho.begin(), g_ortho.end(), resource);
    if (ortho && it == g_ortho.end() && g_ortho.size() < 16) {
        g_ortho.push_back(resource);
        logger::info("[hud] orthographic SceneBuffer %p (UI)", (void*)resource);
    } else if (!ortho && it != g_ortho.end()) {
        g_ortho.erase(it);
    }
}

bool is_ortho(ID3D11Buffer* buffer) {
    std::scoped_lock _{g_ortho_mutex};
    return std::find(g_ortho.begin(), g_ortho.end(), buffer) != g_ortho.end();
}

template <typename T>
void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

bool ensure_texture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& target) {
    if (g_texture != nullptr && g_desc.Width == target.Width && g_desc.Height == target.Height) {
        return true;
    }
    release(g_rtv);
    release(g_texture);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = target.Width;
    desc.Height = target.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // blending in linear, like the HDR scene target
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_texture)) ||
        FAILED(device->CreateRenderTargetView(g_texture, nullptr, &g_rtv))) {
        logger::error("[hud] could not create the HUD texture");
        release(g_texture);
        return false;
    }
    g_desc = desc;
    g_cleared = false;
    logger::info("[hud] HUD texture %ux%u", desc.Width, desc.Height);
    return true;
}

// The game's blend state turned into one that also writes premultiplied coverage
// to alpha (the scene target has no alpha channel).
BlendPair hud_blend(ID3D11Device* device, ID3D11BlendState* original) {
    for (const auto& pair : g_blends) {
        if (pair.original == original) {
            return pair;
        }
    }

    D3D11_BLEND_DESC desc{};
    if (original != nullptr) {
        original->GetDesc(&desc);
    } else {
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    desc.IndependentBlendEnable = FALSE;
    auto& rt = desc.RenderTarget[0];
    bool needs_factor = false;
    if (!rt.BlendEnable) {
        // Opaque UI: replace colour, alpha = 1 (through the blend factor).
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        needs_factor = true;
    } else if (rt.DestBlend == D3D11_BLEND_ONE) {
        // Additive glow: colour adds, coverage unchanged (premultiplied alpha 0 adds).
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
    } else {
        // "Over": coverage accumulates like premultiplied alpha.
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    }
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

    BlendPair pair{original, nullptr, needs_factor};
    if (FAILED(device->CreateBlendState(&desc, &pair.hud))) {
        pair.hud = nullptr;
    }
    if (original != nullptr) {
        original->AddRef(); // keeps the cache key valid
    }
    g_blends.push_back(pair);
    return pair;
}

enum class Kind { other, hud, screen_effect };
Kind g_last_kind = Kind::other;
std::vector<uintptr_t> g_logged_effects;

// Is the next draw a UI draw into the scene target? Returns the target's desc.
bool is_ui_draw(ID3D11DeviceContext* context, D3D11_TEXTURE2D_DESC& target) {
    g_last_kind = Kind::other;
    ID3D11Buffer* scene = nullptr;
    context->VSGetConstantBuffers(0, 1, &scene);
    const bool ortho = scene != nullptr && is_ortho(scene);
    if (scene != nullptr) {
        scene->Release();
    }
    if (!ortho) {
        return false;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv == nullptr) {
        return false;
    }
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    bool full_size_scene = false;
    if (resource != nullptr) {
        D3D11_RESOURCE_DIMENSION dimension{};
        resource->GetType(&dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            static_cast<ID3D11Texture2D*>(resource)->GetDesc(&target);
            // The full-resolution scene target only: the game also draws through the
            // UI camera into half-resolution targets (downsamples feeding bloom,
            // blur and exposure), which must stay untouched.
            if (target.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
                g_full_width = std::max(g_full_width, target.Width);
            }
            full_size_scene = target.Format == DXGI_FORMAT_R11G11B10_FLOAT && target.Width >= k_min_target_width &&
                              target.Width == g_full_width;
        }
        resource->Release();
    }
    if (!full_size_scene) {
        return false;
    }

    // Full-screen quads through the UI camera sample a texture of the target's size.
    ID3D11ShaderResourceView* srvs[8]{};
    context->PSGetShaderResources(0, 8, srvs);
    bool samples_screen = false;
    UINT largest_w = 0, largest_h = 0;
    for (auto srv : srvs) {
        if (srv == nullptr) {
            continue;
        }
        ID3D11Resource* texture = nullptr;
        srv->GetResource(&texture);
        if (texture != nullptr) {
            D3D11_RESOURCE_DIMENSION dimension{};
            texture->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC desc{};
                static_cast<ID3D11Texture2D*>(texture)->GetDesc(&desc);
                samples_screen |= desc.Width == target.Width && desc.Height == target.Height;
                if (desc.Width * desc.Height > largest_w * largest_h) {
                    largest_w = desc.Width;
                    largest_h = desc.Height;
                }
            }
            texture->Release();
        }
        srv->Release();
    }
    ID3D11PixelShader* ps_now = nullptr;
    context->PSGetShader(&ps_now, nullptr, nullptr);
    const auto ps_key = reinterpret_cast<uintptr_t>(ps_now);
    if (ps_now != nullptr) {
        ps_now->Release();
    }
    // A shader that draws small textures (atlases, fonts) is a HUD sprite shader;
    // full-screen-texture draws are HUD only with such a shader (the minimap's land
    // layout). The scene composite, transition fades (a capture of the previous
    // view) and screen effects use other shaders and stay in the scene.
    const bool sprite_shader = std::find(g_sprite_ps.begin(), g_sprite_ps.end(), ps_key) != g_sprite_ps.end();
    if (!samples_screen && !sprite_shader && g_sprite_ps.size() < 64) {
        g_sprite_ps.push_back(ps_key);
    }
    samples_screen = samples_screen && !sprite_shader;

    // Full-screen quads that blend over the image are screen effects (sprint speed
    // lines / radial blur, vignettes): head-locked in VR, so they are removed. The
    // scene composite is an opaque copy and stays.
    if (samples_screen) {
        ID3D11BlendState* blend = nullptr;
        float factor[4];
        UINT mask = 0;
        context->OMGetBlendState(&blend, factor, &mask);
        bool blends = false;
        if (blend != nullptr) {
            D3D11_BLEND_DESC desc{};
            blend->GetDesc(&desc);
            blends = desc.RenderTarget[0].BlendEnable != FALSE;
            blend->Release();
        }
        if (blends) {
            g_last_kind = Kind::screen_effect;
            if (std::find(g_logged_effects.begin(), g_logged_effects.end(), ps_key) == g_logged_effects.end() &&
                g_logged_effects.size() < 32) {
                g_logged_effects.push_back(ps_key);
                logger::info("[hud] full-screen effect (ps %llx) removed while head-tracked", (unsigned long long)ps_key);
            }
        }
    } else {
        g_last_kind = Kind::hud;
    }

    // Log each kind of large-texture UI-camera draw once, to tune this rule.
    if (largest_w * 4 >= target.Width) {
        ID3D11PixelShader* ps = nullptr;
        context->PSGetShader(&ps, nullptr, nullptr);
        const auto key = std::make_tuple(reinterpret_cast<uintptr_t>(ps), largest_w, largest_h);
        if (ps != nullptr) {
            ps->Release();
        }
        if (std::find(g_logged_kinds.begin(), g_logged_kinds.end(), key) == g_logged_kinds.end() &&
            g_logged_kinds.size() < 64) {
            g_logged_kinds.push_back(key);
            logger::info("[hud] UI-camera draw, ps %llx, largest texture %ux%u (target %ux%u): %s",
                         (unsigned long long)std::get<0>(key), largest_w, largest_h, target.Width, target.Height,
                         samples_screen ? "scene" : "HUD");
        }
    }
    return !samples_screen;
}

bool before_draw(ID3D11DeviceContext* context) {
    const Mode mode = g_mode;
    if (mode == Mode::off) {
        return false;
    }
    D3D11_TEXTURE2D_DESC target{};
    const bool ui = is_ui_draw(context, target);
    if (g_last_kind == Kind::screen_effect) {
        return true; // skipped in every head-tracked pass
    }
    if (!ui) {
        return false;
    }
    if (mode == Mode::suppress) {
        return true;
    }

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    const bool ok = device != nullptr && ensure_texture(device, target);
    BlendPair blend{};
    if (ok) {
        context->OMGetBlendState(&g_saved_blend, g_saved_factor, &g_saved_mask);
        blend = hud_blend(device, g_saved_blend);
    }
    if (device != nullptr) {
        device->Release();
    }
    if (!ok || blend.hud == nullptr) {
        if (g_saved_blend != nullptr) {
            release(g_saved_blend);
        }
        return true; // keep the eye image clean even if the HUD can't be captured
    }

    if (!g_cleared) {
        const float transparent[4] = {0, 0, 0, 0};
        context->ClearRenderTargetView(g_rtv, transparent);
        g_cleared = true;
    }
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_saved_rtvs, &g_saved_dsv);
    context->OMSetRenderTargets(1, &g_rtv, g_saved_dsv);
    const float one[4] = {1, 1, 1, 1};
    context->OMSetBlendState(blend.hud, blend.needs_factor ? one : g_saved_factor, g_saved_mask);
    g_redirected = true;
    g_captured_any = true;
    if (!g_logged_first) {
        g_logged_first = true;
        logger::info("[hud] first UI draw captured into the HUD layer");
    }
    return false;
}

void after_draw(ID3D11DeviceContext* context) {
    if (!g_redirected) {
        return;
    }
    g_redirected = false;
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_saved_rtvs, g_saved_dsv);
    context->OMSetBlendState(g_saved_blend, g_saved_factor, g_saved_mask);
    for (auto& rtv : g_saved_rtvs) {
        release(rtv);
    }
    release(g_saved_dsv);
    release(g_saved_blend);
}
} // namespace

void install(ID3D11DeviceContext* context) {
    static bool installed = false;
    if (installed || context == nullptr || !hooks::context::install(context)) {
        return;
    }
    installed = true;
    hooks::context::Observer observer;
    observer.on_map = on_map;
    observer.on_unmap = on_unmap;
    hooks::context::add_observer(observer);
    hooks::context::set_draw_interceptor({before_draw, after_draw});
}

void begin_pass(ID3D11DeviceContext* context, Mode mode) {
    if (!g_enabled) {
        mode = Mode::off;
    }
    if (mode == Mode::capture) {
        g_captured_any = false;
        g_cleared = false;
        if (g_rtv != nullptr) {
            const float transparent[4] = {0, 0, 0, 0};
            context->ClearRenderTargetView(g_rtv, transparent);
            g_cleared = true;
        }
    }
    g_mode = mode;
}

void end_pass() {
    g_mode = Mode::off;
}

ID3D11Texture2D* texture() {
    return g_texture;
}

bool captured_any() {
    return g_captured_any;
}

void on_present() {
    const bool down = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    if (down && !g_key_was_down) {
        g_enabled = !g_enabled;
        logger::info("[hud] Home: HUD layer %s", g_enabled ? "on" : "off (HUD stays in the eye images)");
    }
    g_key_was_down = down;
}

bool enabled() {
    return g_enabled;
}
} // namespace game::hud
