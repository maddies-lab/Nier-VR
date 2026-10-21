#include "game/camera_hook.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <windows.h>

#include <MinHook.h>

#include "game/camera.hpp"
#include "game/player.hpp"
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
constexpr size_t k_camera_data_size = 0x2A0; // stride of the game's CameraData array

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
bool g_was_player_down = false;

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

// The game camera's frame as the head pose is applied to it. With pitch lock only
// the game camera's yaw is kept, so the horizon stays level.
struct CameraBasis {
    math::Vec3 right, up, back;
    float distance = 1.0f;

    math::Vec3 to_world(const math::Vec3& v) const { return right * v.x + up * v.y + back * v.z; }

    // Eye/target/up for a view whose pose is relative to the recentered reference
    // (OpenXR view-space axes = the camera's local axes).
    void place(const xr::RenderView& view, const Vec4& eye, const Vec4& target, const Vec4& up_in, Vec4& out_eye,
               Vec4& out_target, Vec4& out_up) const {
        const math::Vec3 forward = to_world(math::rotate(view.head_orientation, {0.0f, 0.0f, -1.0f}));
        const math::Vec3 head_up = to_world(math::rotate(view.head_orientation, {0.0f, 1.0f, 0.0f}));
        const math::Vec3 position = xyz(eye) + to_world(view.head_position) * k_world_scale;
        out_eye = with_w(position, eye.w);
        out_target = with_w(position + forward * distance, target.w);
        out_up = with_w(head_up, up_in.w);
    }
};

CameraBasis camera_basis(const Vec4& eye, const Vec4& target, const Vec4& up) {
    const math::Vec3 to_target = xyz(target) - xyz(eye);
    CameraBasis basis;
    basis.distance = math::length(to_target) > 0.001f ? math::length(to_target) : 1.0f;
    math::Vec3 forward = math::normalize(to_target);
    math::Vec3 world_up = math::normalize(xyz(up));
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
    basis.back = forward * -1.0f;
    basis.right = math::normalize(math::cross(world_up, basis.back));
    basis.up = math::cross(basis.back, basis.right);
    return basis;
}

// Yaw speed of the game camera (stick turns, follow-camera swings), smoothed, for
// the comfort vignette. Jumps over 60 degrees in one update are camera cuts.
void measure_turn_rate(const CameraBasis& basis) {
    static float last_yaw = 0.0f;
    static LARGE_INTEGER last_time{};
    static float rate = 0.0f;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float yaw = std::atan2(basis.back.x, basis.back.z);
    if (last_time.QuadPart != 0) {
        const float dt = static_cast<float>(now.QuadPart - last_time.QuadPart) / frequency.QuadPart;
        const float delta = std::fabs(std::remainder(yaw - last_yaw, 2.0f * 3.14159265f));
        if (dt > 0.0f && dt < 0.25f && delta < 1.05f) {
            const float instant = delta / dt * 57.29578f;
            rate += (instant - rate) * 0.3f;
        }
    }
    last_yaw = yaw;
    last_time = now;
    xr::set_game_turn_rate(rate);
}

// The fov/aspect the game rebuilds its projection and culling frustum from, while
// the original update runs.
void set_culling_fov(CameraData* data, const xr::RenderView& view) {
    const float half_width = std::max(-view.tan_left, view.tan_right);
    const float half_height = std::max(view.tan_up, -view.tan_down);
    field<float>(data, k_fov) = 2.0f * std::atan(half_height);
    field<float>(data, k_aspect) = half_width / half_height;
    field<int>(data, k_projection_dirty) = 1;
}

// The view's projection (x/y terms; the game's depth terms stay) and view*projection.
void set_projection(CameraData* data, const xr::RenderView& view) {
    auto& projection = matrix(data, k_projection);
    projection.m[0][0] = 2.0f / (view.tan_right - view.tan_left);
    projection.m[1][1] = 2.0f / (view.tan_up - view.tan_down);
    projection.m[2][0] = (view.tan_right + view.tan_left) / (view.tan_right - view.tan_left);
    projection.m[2][1] = (view.tan_up + view.tan_down) / (view.tan_up - view.tan_down);
    matrix(data, k_view_projection) = math::multiply(matrix(data, 0x00), projection);
}

// Both-eyes mode: the right eye's camera, computed by the game's own update on a
// scratch copy of CameraData (frustum update off, so nothing shared is touched), and
// handed over in SceneBuffer layout: CameraData+0x00..0xFF, each matrix transposed.
void build_secondary_scene(const CameraData* data, const CameraBasis& basis, const xr::RenderView& view,
                           const Vec4& eye, const Vec4& target, const Vec4& up) {
    alignas(16) static uint8_t storage[k_camera_data_size];
    std::memcpy(storage, data, k_camera_data_size);
    auto scratch = reinterpret_cast<CameraData*>(storage);
    CameraData* scratch_holder = scratch;

    Vec4 vr_eye, vr_target, vr_up;
    basis.place(view, eye, target, up, vr_eye, vr_target, vr_up);
    set_culling_fov(scratch, view);
    g_original(&scratch_holder, &vr_eye, &vr_target, &vr_up, 0, 0);
    set_projection(scratch, view);

    float scene[64];
    for (int k = 0; k < 4; ++k) {
        const auto& m = matrix(scratch, k * 0x40);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                scene[k * 16 + r * 4 + c] = m.m[c][r];
            }
        }
    }
    xr::set_secondary_scene(scene);
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

    // Game over: the camera keeps mode 0 and keeps running, but the player is at 0 HP.
    const auto health = player::health();
    const bool player_down = health && *health == 0;
    if (player_down != g_was_player_down) {
        logger::info("[camera_hook] player %s", player_down ? "down (theater)" : "alive");
        g_was_player_down = player_down;
    }

    // The game's own fov/aspect: our wide values are only present during the
    // original call below, because the camera controller eases from the stored fov
    // and per-object LOD reads it (NieRVR's notes found both).
    const float game_fov = field<float>(data, k_fov);
    const float game_aspect = field<float>(data, k_aspect);

    xr::trace("update");
    xr::RenderView view, second_view;
    const int views = (game_aspect <= 0.0f || is_theater_mode(mode) || paused || player_down)
                          ? 0
                          : xr::begin_render_views(game_aspect, view, second_view);
    if (views == 0) {
        xr::set_game_turn_rate(0.0f);
        if (g_projection_is_ours) {
            field<int>(data, k_projection_dirty) = 1; // rebuild the game's own projection
            g_projection_is_ours = false;
        }
        g_have_last = false;
        g_original(holder, eye, target, up, save_previous, update_frustum);
        return;
    }

    const auto basis = camera_basis(*eye, *target, *up);
    measure_turn_rate(basis);
    Vec4 vr_eye, vr_target, vr_up;
    basis.place(view, *eye, *target, *up, vr_eye, vr_target, vr_up);

    // A symmetric frustum that contains the eye's, so culling keeps everything the
    // headset can see.
    set_culling_fov(data, view);

    // Motion blur / TAA compare against the previous frame as it was rendered.
    if (save_previous && g_have_last) {
        matrix(data, k_previous_view_projection) = g_last_view_projection;
        save_previous = 0;
    }

    g_original(holder, &vr_eye, &vr_target, &vr_up, save_previous, update_frustum);
    field<float>(data, k_fov) = game_fov;
    field<float>(data, k_aspect) = game_aspect;

    set_projection(data, view);
    g_last_view_projection = matrix(data, k_view_projection);
    g_have_last = true;
    g_projection_is_ours = true;

    if (views == 2) {
        build_secondary_scene(data, basis, second_view, *eye, *target, *up);
    }
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
