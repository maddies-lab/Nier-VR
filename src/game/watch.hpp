#pragma once

#include <cstdint>

// Reverse-engineering helper: hardware data breakpoints (debug registers) on up to
// four addresses across all game threads. Every hit's instruction address is
// counted and written to NieR-VR.log when the watch ends.
namespace game::watch {
enum class Access : uint8_t {
    write = 1,      // DR7 RW=01
    read_write = 3, // DR7 RW=11
};

struct Target {
    const char* label;
    uintptr_t address; // must be 8-byte aligned
    Access access;
};

// Arms the targets for `duration_ms`, then disarms them and logs the results.
// Runs on its own thread; returns immediately. Ignored while a watch is active.
void start(const Target (&targets)[4], uint32_t duration_ms);

// Arms the CameraData matrix rows and the camera eye (bound to F10).
void start_camera_watch();
} // namespace game::watch
