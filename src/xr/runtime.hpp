#pragma once

#include <cstdint>
#include <string>

#include <dxgi.h>

#include <d3d11.h>

#include "math.hpp"
#include "xr/frame_transport.hpp"

// OpenXR session and the M1 "theater" presentation: the game's backbuffer is
// shown on a world-locked quad in front of the player.
namespace xr {
struct Timing {
    float avg_ms = 0;
    float max_ms = 0;
};

struct Status {
    std::string runtime;         // e.g. "WiVRn 26.9.0"
    std::string system;          // headset name
    const char* state = "not started";
    const char* transport = "-"; // how frames cross from the game's device to the XR device
    bool head_tracking = false;  // F7
    bool projected = false;      // last submitted frame used a projection layer
    const char* presentation = "none"; // "theater", "mono" or "stereo"
    const char* stereo_mode = "";  // F5
    int pairing = 1;             // F6: 1 = pose from the previous Present, 0 = newest
    std::string last_error;
    uint32_t width = 0, height = 0;

    // Rolling one-second windows.
    float headset_fps = 0;       // xrEndFrame calls
    float game_fps = 0;          // game frames handed to the XR device
    float dropped_fps = 0;       // game frames that could not be handed over (errors only)
    Timing xr_wait, xr_begin, xr_copy, xr_end; // XR thread
    Timing game_copy;                          // render thread: backbuffer -> shared texture
};

// Call from the game's Present (render thread). Starts the XR thread on first use.
void on_present(IDXGISwapChain* swapchain);

// Places the screen in front of the current head pose on the next frame; in
// head-tracked mode, also makes the current head yaw the game camera's forward.
void request_recenter();

// Head-tracked rendering (M2). F7 toggles it.
struct RenderView {
    math::Quat head_orientation; // relative to the recentered reference (yaw removed)
    math::Vec3 head_position;    // metres, relative to the reference, in the reference's frame
    // Render frustum as tangents (left/down negative), widened to the game's aspect.
    float tan_left = 0, tan_right = 0, tan_up = 0, tan_down = 0;
};

// Game thread (main-camera update). When head tracking is on and the headset is
// tracking, fills the view(s) to render and remembers their poses for this frame's
// submission. Returns 0 (render normally), 1 (primary only: mono, or one eye in
// alternate mode) or 2 (both eyes: primary = left, secondary = right).
int begin_render_views(float aspect, RenderView& primary, RenderView& secondary);

// Game thread, both-eyes mode: the right eye's SceneBuffer block (View, Proj,
// ViewProj, ViewInverse as the shaders read them) for the frame just begun.
void set_secondary_scene(const float (&scene)[64]);

// Draw-job thread (both-eyes mode). The views of the render queue being drawn.
struct JobViews {
    uint64_t sequence = 0;
    transport::FrameMeta primary;
    float secondary_scene[64]{};
};
bool job_views(JobViews& out);
// Copies the backbuffer (the left eye, after the first pass) to the headset.
bool publish_from_job(const transport::FrameMeta& meta);
// The second pass ran: the backbuffer at Present is the right eye.
void mark_both_rendered(uint64_t sequence);

ID3D11DeviceContext* game_context();
const char* stereo_mode_name(int mode);

Status status();

// Logs a timestamped event with its thread for a short window after F7 (frame-order debugging).
void trace(const char* event);
} // namespace xr
