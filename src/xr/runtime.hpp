#pragma once

#include <cstdint>
#include <string>

#include <dxgi.h>

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

// Places the screen in front of the current head pose on the next frame.
void request_recenter();

Status status();
} // namespace xr
