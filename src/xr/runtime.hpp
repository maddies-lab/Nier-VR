#pragma once

#include <cstdint>
#include <string>

#include <dxgi.h>

#include "math.hpp"

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

// Game thread. When head tracking is on and the headset is tracking, samples the head
// pose predicted for this frame's display and returns the view to render. The pose is
// remembered and sent with this frame to the compositor.
bool begin_render_view(float aspect, RenderView& view);

Status status();

// Logs a timestamped event with its thread for a short window after F7 (frame-order debugging).
void trace(const char* event);
} // namespace xr
