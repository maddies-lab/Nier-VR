#include "game/camera_hook.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <windows.h>

#include <MinHook.h>

#include "game/camera.hpp"
#include "log.hpp"
#include "math.hpp"
#include "scan.hpp"
#include "xr/runtime.hpp"

namespace game::camera_hook {
namespace {
// Keep the game camera's pitch out of the view: the player's head does the pitching,
// so the horizon stays level.
constexpr bool k_pitch_lock = true;
constexpr float k_world_scale = 1.0f; // game units per metre of head movement

// UpdateCameraData(CameraData** holder, eye, target, up, save_previous, update_frustum)
using UpdateFn = void(__fastcall*)(CameraData** holder, const Vec4* eye, const Vec4* target, const Vec4* up,
                                   int save_previous, int update_frustum);
UpdateFn g_original = nullptr;

// CameraData fields used here (see the RE notes in the M2 commit).
constexpr uintptr_t k_projection = 0x40;
constexpr uintptr_t k_view_projection = 0x80;
constexpr uintptr_t k_previous_view_projection = 0x200;
constexpr uintptr_t k_aspect = 0x270;
constexpr uintptr_t k_fov = 0x274;
constexpr uintptr_t k_projection_dirty = 0x284;

// Main camera state across frames (game thread).
bool g_overriding = false;         // CameraData currently holds our fov/aspect
float g_game_fov = 0, g_game_aspect = 0;
float g_written_fov = 0, g_written_aspect = 0;
math::Mat4 g_last_view_projection{};
bool g_have_last = false;
float g_last_yaw_forward[2] = {0.0f, -1.0f}; // fallback when the camera looks straight up/down

template <typename T>
T& field(CameraData* data, uintptr_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(data) + offset);
}

math::Mat4& matrix(CameraData* data, uintptr_t offset) {
    return field<math::Mat4>(data, offset);
}

math::Vec3 xyz(const Vec4& v) {
    return {v.x, v.y, v.z};
}

Vec4 with_w(const math::Vec3& v, float w) {
    return {v.x, v.y, v.z, w};
}

// Gives the game back its own fov/aspect unless it changed them itself meanwhile.
void restore_game_projection(CameraData* data) {
    if (!g_overriding) {
        return;
    }
    if (field<float>(data, k_fov) == g_written_fov) {
        field<float>(data, k_fov) = g_game_fov;
    }
    if (field<float>(data, k_aspect) == g_written_aspect) {
        field<float>(data, k_aspect) = g_game_aspect;
    }
    field<int>(data, k_projection_dirty) = 1;
    g_overriding = false;
}

void __fastcall update_detour(CameraData** holder, const Vec4* eye, const Vec4* target, const Vec4* up,
                              int save_previous, int update_frustum) {
    const auto camera = CameraGame::get();
    CameraData* data = *holder;
    if (camera == nullptr || data == nullptr || camera->data() != data) {
        g_original(holder, eye, target, up, save_previous, update_frustum);
        return;
    }

    // The game's own projection parameters for this frame.
    if (!g_overriding || field<float>(data, k_fov) != g_written_fov) {
        g_game_fov = field<float>(data, k_fov);
    }
    if (!g_overriding || field<float>(data, k_aspect) != g_written_aspect) {
        g_game_aspect = field<float>(data, k_aspect);
    }

    xr::trace("update");
    xr::RenderView view;
    if (g_game_aspect <= 0.0f || !xr::begin_render_view(g_game_aspect, view)) {
        restore_game_projection(data);
        g_have_last = false;
        g_original(holder, eye, target, up, save_previous, update_frustum);
        return;
    }

    // Game camera basis. With pitch lock, only its yaw is kept.
    const math::Vec3 game_eye = xyz(*eye);
    const math::Vec3 to_target = xyz(*target) - game_eye;
    const float distance = math::length(to_target) > 0.001f ? math::length(to_target) : 1.0f;
    math::Vec3 forward = math::normalize(to_target);
    math::Vec3 world_up = math::normalize(xyz(*up));
    if (k_pitch_lock) {
        math::Vec3 flat{forward.x, 0.0f, forward.z};
        if (math::length(flat) < 1e-3f) {
            flat = {g_last_yaw_forward[0], 0.0f, g_last_yaw_forward[1]};
        }
        forward = math::normalize(flat);
        g_last_yaw_forward[0] = forward.x;
        g_last_yaw_forward[1] = forward.z;
        world_up = {0.0f, 1.0f, 0.0f};
    }
    const math::Vec3 back = forward * -1.0f;
    const math::Vec3 right = math::normalize(math::cross(world_up, back));
    const math::Vec3 camera_up = math::cross(back, right);
    const auto to_world = [&](const math::Vec3& v) { return right * v.x + camera_up * v.y + back * v.z; };

    // Head pose applied in the camera's local frame (same axes as OpenXR's view space).
    const math::Vec3 head_forward = to_world(math::rotate(view.head_orientation, {0.0f, 0.0f, -1.0f}));
    const math::Vec3 head_up = to_world(math::rotate(view.head_orientation, {0.0f, 1.0f, 0.0f}));
    const math::Vec3 new_eye = game_eye + to_world(view.head_position) * k_world_scale;
    const Vec4 vr_eye = with_w(new_eye, eye->w);
    const Vec4 vr_target = with_w(new_eye + head_forward * distance, target->w);
    const Vec4 vr_up = with_w(head_up, up->w);

    // A symmetric frustum that contains the asymmetric one, so culling keeps
    // everything the headset can see.
    const float half_width = std::max(-view.tan_left, view.tan_right);
    const float half_height = std::max(view.tan_up, -view.tan_down);
    g_written_fov = 2.0f * std::atan(half_height);
    g_written_aspect = half_width / half_height;
    field<float>(data, k_fov) = g_written_fov;
    field<float>(data, k_aspect) = g_written_aspect;
    field<int>(data, k_projection_dirty) = 1;
    g_overriding = true;

    // Motion blur / TAA compare against the previous frame as it was rendered.
    if (save_previous && g_have_last) {
        matrix(data, k_previous_view_projection) = g_last_view_projection;
        save_previous = 0;
    }

    g_original(holder, &vr_eye, &vr_target, &vr_up, save_previous, update_frustum);

    // Swap the symmetric projection for the headset's asymmetric one (x/y terms only;
    // the game's depth terms stay) and rebuild view*projection.
    auto& projection = matrix(data, k_projection);
    projection.m[0][0] = 2.0f / (view.tan_right - view.tan_left);
    projection.m[1][1] = 2.0f / (view.tan_up - view.tan_down);
    projection.m[2][0] = (view.tan_right + view.tan_left) / (view.tan_right - view.tan_left);
    projection.m[2][1] = (view.tan_up + view.tan_down) / (view.tan_up - view.tan_down);
    auto& view_projection = matrix(data, k_view_projection);
    view_projection = math::multiply(matrix(data, 0x00), projection);
    g_last_view_projection = view_projection;
    g_have_last = true;
}
} // namespace

bool install() {
    const auto exe = GetModuleHandleW(nullptr);
    const auto text = scan::section(exe, ".text");
    if (!text) {
        return false;
    }

    // Prologue of UpdateCameraData, unique in the 2021 Steam (Compat) build.
    const auto target = scan::find_pattern(
        text->start, text->size,
        "48 8B C4 48 89 68 18 56 57 41 56 48 81 EC ?? ?? ?? ?? 83 BC 24 ?? ?? ?? ?? 00 49 8B F1");
    if (!target) {
        logger::error("[camera_hook] UpdateCameraData not found; head tracking unavailable");
        return false;
    }

    const auto address = reinterpret_cast<void*>(*target);
    if (MH_CreateHook(address, reinterpret_cast<void*>(&update_detour), reinterpret_cast<void**>(&g_original)) !=
            MH_OK ||
        MH_EnableHook(address) != MH_OK) {
        logger::error("[camera_hook] could not hook UpdateCameraData");
        return false;
    }

    logger::info("[camera_hook] UpdateCameraData hooked at exe+0x%llx",
                 (unsigned long long)(*target - reinterpret_cast<uintptr_t>(exe)));
    return true;
}
} // namespace game::camera_hook
