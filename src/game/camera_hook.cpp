#include "game/camera_hook.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

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
bool g_projection_is_ours = false; // CameraData+0x40 holds the headset projection
math::Mat4 g_last_view_projection{};
bool g_have_last = false;
float g_last_yaw_forward[2] = {0.0f, -1.0f}; // fallback when the camera looks straight up/down

// cCameraGame+0x390 values whose screens stay on the theater screen (the user's
// 2026-10-17 sessions): 44 (0x2C) = no player to follow (intro cutscene).
// Other modes seen: 0 = gameplay (also teleport cutscenes), 3 = the hub's 2D
// side-scrolling camera (head-tracked). Menus and maps pause the game camera and
// are caught by the paused check instead.
constexpr int k_theater_modes[] = {44};

// cCameraGame's per-frame update (vtable slot 2). While the game is paused (pause
// menu and its map) it stops running, but the map still drives the camera matrices.
using CameraUpdateFn = void(__fastcall*)(void* camera);
CameraUpdateFn g_camera_update_original = nullptr;
std::atomic<uint64_t> g_camera_update_ms{0};
constexpr uint64_t k_paused_after_ms = 50;
bool g_was_paused = false;

void __fastcall camera_update_detour(void* camera) {
    g_camera_update_ms = GetTickCount64();
    g_camera_update_original(camera);
}

bool game_paused() {
    const uint64_t last = g_camera_update_ms;
    return last != 0 && GetTickCount64() - last > k_paused_after_ms;
}
int g_last_main_mode = -1;

int main_camera_mode(const CameraGame* camera) {
    return *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(camera) + 0x390);
}

bool is_theater_mode(int mode) {
    return std::find(std::begin(k_theater_modes), std::end(k_theater_modes), mode) != std::end(k_theater_modes);
}

// RE log: each distinct (caller, CameraData, cCameraGame mode) combination, so
// screens like the pause menu's 3D map can be told apart from gameplay.
struct Signature {
    uintptr_t caller = 0;
    const void* data = nullptr;
    int mode = -1;
    bool operator==(const Signature& o) const { return caller == o.caller && data == o.data && mode == o.mode; }
};
std::vector<Signature> g_seen_signatures;

void log_signature(uintptr_t return_address, const void* data, bool main_camera) {
    const auto camera = CameraGame::get();
    const int mode = camera != nullptr ? *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(camera) + 0x390) : -1;
    const Signature signature{return_address, data, main_camera ? mode : -1};
    if (g_seen_signatures.size() >= 64 ||
        std::find(g_seen_signatures.begin(), g_seen_signatures.end(), signature) != g_seen_signatures.end()) {
        return;
    }
    g_seen_signatures.push_back(signature);
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    logger::info("[camera_hook] new: update from exe+0x%llx, data %p (%s), camera mode %d",
                 (unsigned long long)(return_address - exe), data, main_camera ? "main" : "other", mode);
}

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

void __fastcall update_detour(CameraData** holder, const Vec4* eye, const Vec4* target, const Vec4* up,
                              int save_previous, int update_frustum) {
    const auto camera = CameraGame::get();
    CameraData* data = *holder;
    log_signature(reinterpret_cast<uintptr_t>(__builtin_return_address(0)), data,
                  camera != nullptr && camera->data() == data);
    if (camera == nullptr || data == nullptr || camera->data() != data) {
        g_original(holder, eye, target, up, save_previous, update_frustum);
        return;
    }

    static int update_count = 0;
    if (++update_count == 600) {
        logger::backtrace("UpdateCameraData (main camera)");
    }

    const int mode = main_camera_mode(camera);
    if (mode != g_last_main_mode) {
        logger::info("[camera_hook] main camera mode %d -> %d%s", g_last_main_mode, mode,
                     is_theater_mode(mode) ? " (theater)" : "");
        g_last_main_mode = mode;
    }
    const bool paused = game_paused();
    if (paused != g_was_paused) {
        logger::info("[camera_hook] game camera %s", paused ? "paused (theater)" : "running");
        g_was_paused = paused;
    }

    // The game's own fov/aspect: our wide values are only present during the
    // original call below, because the camera controller eases from the stored fov
    // and per-object LOD reads it (NieRVR's notes found both).
    const float game_fov = field<float>(data, k_fov);
    const float game_aspect = field<float>(data, k_aspect);

    xr::trace("update");
    xr::RenderView view;
    if (game_aspect <= 0.0f || is_theater_mode(mode) || paused || !xr::begin_render_view(game_aspect, view)) {
        if (g_projection_is_ours) {
            field<int>(data, k_projection_dirty) = 1; // rebuild the game's own projection
            g_projection_is_ours = false;
        }
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
    field<float>(data, k_fov) = 2.0f * std::atan(half_height);
    field<float>(data, k_aspect) = half_width / half_height;
    field<int>(data, k_projection_dirty) = 1;

    // Motion blur / TAA compare against the previous frame as it was rendered.
    if (save_previous && g_have_last) {
        matrix(data, k_previous_view_projection) = g_last_view_projection;
        save_previous = 0;
    }

    g_original(holder, &vr_eye, &vr_target, &vr_up, save_previous, update_frustum);
    field<float>(data, k_fov) = game_fov;
    field<float>(data, k_aspect) = game_aspect;

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
    g_projection_is_ours = true;
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

    // The pause detector is optional: without it the pause-menu map stays head-tracked.
    const auto update = reinterpret_cast<void**>(CameraGame::vtable())[2];
    if (MH_CreateHook(update, reinterpret_cast<void*>(&camera_update_detour),
                      reinterpret_cast<void**>(&g_camera_update_original)) == MH_OK &&
        MH_EnableHook(update) == MH_OK) {
        logger::info("[camera_hook] cCameraGame update hooked at exe+0x%llx",
                     (unsigned long long)(reinterpret_cast<uintptr_t>(update) - reinterpret_cast<uintptr_t>(exe)));
    } else {
        logger::warn("[camera_hook] could not hook the cCameraGame update; pause detection off");
    }
    return true;
}
} // namespace game::camera_hook
