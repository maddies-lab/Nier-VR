#include "game/stereo_render.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#include <windows.h>

#include <MinHook.h>

#include "game/camera.hpp"
#include "hooks/context_hook.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "xr/runtime.hpp"

namespace game::stereo_render {
namespace {
constexpr UINT k_scene_buffer_size = 256;

using DrawJobFn = void(__fastcall*)(void* device);
DrawJobFn g_draw_job = nullptr;

// The main camera's SceneBuffer: the 256-byte constant buffer whose upload equals
// CameraData+0x00..0xFF transposed. Found again whenever the upload stops matching.
std::atomic<ID3D11Resource*> g_scene_buffer{nullptr};
thread_local ID3D11Resource* t_mapped_resource = nullptr;
thread_local const float* t_mapped_data = nullptr;
bool g_registered = false;

// Uploaded ViewProj (SceneBuffer +0x80, transposed) == CameraData+0x80?
bool matches_main_camera(const float* scene) {
    const auto camera = CameraGame::get();
    if (camera == nullptr || camera->data() == nullptr) {
        return false;
    }
    const auto view_projection = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(camera->data()) + 0x80);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float uploaded = scene[32 + r * 4 + c];
            const float expected = view_projection[c * 4 + r];
            if (std::fabs(uploaded - expected) > 1e-3f * std::fmax(1.0f, std::fabs(expected))) {
                return false;
            }
        }
    }
    return true;
}

void on_map(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT hr) {
    t_mapped_resource = nullptr;
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
        t_mapped_resource = resource;
        t_mapped_data = static_cast<const float*>(mapped->pData);
    }
}

void on_unmap(ID3D11Resource* resource) {
    if (resource != t_mapped_resource || t_mapped_data == nullptr) {
        return;
    }
    t_mapped_resource = nullptr;
    if (matches_main_camera(t_mapped_data) && g_scene_buffer.exchange(resource) != resource) {
        logger::info("[stereo] main camera SceneBuffer is %p", (void*)resource);
    }
}

std::atomic<uint64_t> g_doubled{0}, g_single{0};

void __fastcall draw_job_detour(void* device) {
    xr::JobViews views;
    const auto scene_buffer = g_scene_buffer.load();
    const auto context = xr::game_context();
    if (scene_buffer == nullptr || context == nullptr || !xr::job_views(views)) {
        ++g_single;
        g_draw_job(device);
        return;
    }

    // Left eye: the queue as recorded and uploaded.
    g_draw_job(device);
    xr::publish_from_job(views.primary);

    // Right eye: same queue, the main camera's SceneBuffer re-uploaded.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(hooks::context::map_original(context, scene_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, views.secondary_scene, sizeof(views.secondary_scene));
    hooks::context::unmap_original(context, scene_buffer, 0);
    g_draw_job(device);
    xr::mark_both_rendered(views.sequence);

    if (++g_doubled == 1) {
        logger::info("[stereo] first frame drawn for both eyes");
    }
}
} // namespace

bool install() {
    const auto exe = GetModuleHandleW(nullptr);
    const auto text = scan::section(exe, ".text");
    if (!text) {
        return false;
    }

    // The draw job: begin, execute the render queue, end.
    const auto target = scan::find_pattern(text->start, text->size,
                                           "40 53 48 83 EC 20 48 8B D9 B9 01 00 00 00 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? "
                                           "?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 48 8D 0D");
    if (!target) {
        logger::error("[stereo] draw job not found; both-eyes rendering unavailable");
        return false;
    }

    const auto address = reinterpret_cast<void*>(*target);
    if (MH_CreateHook(address, reinterpret_cast<void*>(&draw_job_detour), reinterpret_cast<void**>(&g_draw_job)) !=
            MH_OK ||
        MH_EnableHook(address) != MH_OK) {
        logger::error("[stereo] could not hook the draw job");
        return false;
    }
    logger::info("[stereo] draw job hooked at exe+0x%llx",
                 (unsigned long long)(*target - reinterpret_cast<uintptr_t>(exe)));
    return true;
}

void on_present(ID3D11DeviceContext* context) {
    if (!g_registered && context != nullptr && hooks::context::install(context)) {
        hooks::context::add_observer({on_map, on_unmap, nullptr});
        g_registered = true;
    }
}
} // namespace game::stereo_render
