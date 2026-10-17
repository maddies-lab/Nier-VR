#include "overlay.hpp"

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "game/build.hpp"
#include "game/camera.hpp"
#include "game/inspect.hpp"
#include "game/watch.hpp"
#include "log.hpp"
#include "xr/runtime.hpp"

namespace overlay {
namespace {
bool g_initialized = false;
bool g_visible = true;
bool g_toggle_was_down = false;
bool g_snapshot_was_down = false;
bool g_watch_was_down = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_last_frame{};
uint64_t g_frame = 0;
game::BuildInfo g_build{};

bool initialize(IDXGISwapChain* swapchain) {
    if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device)))) {
        logger::error("[overlay] swapchain has no D3D11 device");
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui_ImplDX11_Init(g_device, g_context);

    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_last_frame);
    g_build = game::identify_build();
    logger::info("[overlay] initialized");
    return true;
}

bool ensure_render_target(IDXGISwapChain* swapchain) {
    if (g_rtv != nullptr) {
        return true;
    }

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) {
        return false;
    }
    const auto hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

void matrix_rows(const char* label, const game::Mat4& m) {
    ImGui::TextUnformatted(label);
    static const char* k_row_names[] = {"right  ", "up     ", "forward", "origin "};
    for (int i = 0; i < 4; ++i) {
        const auto& r = m.rows[i];
        ImGui::Text("  %s %9.3f %9.3f %9.3f %9.3f", k_row_names[i], r.x, r.y, r.z, r.w);
    }
}

void draw_window(IDXGISwapChain* swapchain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);

    ImGui::SetNextWindowPos(ImVec2{16, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("NieR-VR  M1", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("Insert: overlay  F5: stereo/mono  F6: pose pairing  F7: head tracking  F8: recenter  F9/F10: RE");
    ImGui::Separator();
    ImGui::Text("Build: %s (stamp 0x%08X)", g_build.name != nullptr ? g_build.name : "UNKNOWN", g_build.timestamp);
    ImGui::Text("Backbuffer: %u x %u, format %d, frame %llu", desc.BufferDesc.Width, desc.BufferDesc.Height,
                (int)desc.BufferDesc.Format, (unsigned long long)g_frame);
    ImGui::Text("Frame time: %.2f ms", ImGui::GetIO().DeltaTime * 1000.0f);
    ImGui::Separator();

    const auto vr = xr::status();
    ImGui::Text("OpenXR: %s   F7 head tracking: %s   F5 render: %s   showing: %s", vr.state,
                vr.head_tracking ? "on" : "off", vr.stereo ? "stereo (alternate eyes)" : "mono", vr.presentation);
    if (vr.head_tracking) {
        ImGui::Text("  F6 pose pairing: %s", vr.pairing ? "previous Present (pipelined)" : "newest");
    }
    if (!vr.runtime.empty()) {
        ImGui::Text("  runtime %s, headset %s", vr.runtime.c_str(), vr.system.empty() ? "-" : vr.system.c_str());
    }
    ImGui::Text("  transport: %s, screen %ux%u", vr.transport, vr.width, vr.height);
    ImGui::Text("  headset %5.1f fps  wait %5.2f/%5.2f  begin %4.2f/%4.2f  copy %4.2f/%4.2f  end %5.2f/%5.2f ms",
                vr.headset_fps, vr.xr_wait.avg_ms, vr.xr_wait.max_ms, vr.xr_begin.avg_ms, vr.xr_begin.max_ms,
                vr.xr_copy.avg_ms, vr.xr_copy.max_ms, vr.xr_end.avg_ms, vr.xr_end.max_ms);
    ImGui::Text("  game    %5.1f fps  dropped %4.1f fps  copy %4.2f/%4.2f ms (avg/max)", vr.game_fps,
                vr.dropped_fps, vr.game_copy.avg_ms, vr.game_copy.max_ms);
    if (!vr.last_error.empty()) {
        ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "  %s", vr.last_error.c_str());
    }
    ImGui::Separator();

    const auto camera = game::CameraGame::get();
    if (camera == nullptr) {
        ImGui::TextColored(ImVec4{1, 0.6f, 0.3f, 1}, "cCameraGame: not found yet");
    } else if (!camera->valid()) {
        ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "cCameraGame %p: vftable mismatch", (void*)camera);
    } else {
        const auto& p = camera->position();
        ImGui::Text("cCameraGame %p", (void*)camera);
        ImGui::Text("position  %9.3f %9.3f %9.3f", p.x, p.y, p.z);
        ImGui::Text("fov       %.4f rad (%.2f deg)", camera->fov(), camera->fov() * 57.29578f);
        if (const auto data = camera->data()) {
            matrix_rows("view (camera-to-world)", data->view);
        } else {
            ImGui::Text("CameraData: null");
        }
    }

    ImGui::End();
}
} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_initialized) {
        g_initialized = initialize(swapchain);
        if (!g_initialized) {
            return;
        }
    }

    ++g_frame;
    const bool toggle_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (toggle_down && !g_toggle_was_down) {
        g_visible = !g_visible;
    }
    g_toggle_was_down = toggle_down;

    const bool snapshot_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (snapshot_down && !g_snapshot_was_down) {
        game::log_camera_snapshot();
    }
    g_snapshot_was_down = snapshot_down;

    const bool watch_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (watch_down && !g_watch_was_down) {
        game::watch::start_camera_watch();
    }
    g_watch_was_down = watch_down;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    auto& io = ImGui::GetIO();
    io.DeltaTime = static_cast<float>(now.QuadPart - g_last_frame.QuadPart) / g_frequency.QuadPart;
    if (io.DeltaTime <= 0.0f) {
        io.DeltaTime = 1.0f / 60.0f;
    }
    g_last_frame = now;

    if (!g_visible || !ensure_render_target(swapchain)) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    io.DisplaySize = ImVec2{static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height)};

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    draw_window(swapchain);
    ImGui::Render();

    ID3D11RenderTargetView* saved_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* saved_dsv = nullptr;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, &saved_dsv);

    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved_rtvs, saved_dsv);
    for (auto rtv : saved_rtvs) {
        if (rtv != nullptr) {
            rtv->Release();
        }
    }
    if (saved_dsv != nullptr) {
        saved_dsv->Release();
    }
}

void on_pre_resize(IDXGISwapChain*) {
    if (g_rtv != nullptr) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}
} // namespace overlay
