#pragma once

#include <atomic>

// Settings in NieR-VR.ini next to the game executable. Read at start; values the
// player changes with hotkeys are written back.
namespace config {
struct Hud {
    std::atomic<float> width{2.0f};          // metres
    std::atomic<float> distance{1.8f};       // metres in front of the head
    std::atomic<float> drop{0.15f};          // metres below eye level
    std::atomic<float> follow_degrees{35.0f}; // head turn before the panel follows
};

struct Comfort {
    std::atomic<bool> vignette{true};
    std::atomic<float> strength{0.85f};      // darkness at full turn speed (0..1)
    std::atomic<float> start_speed{40.0f};   // game-camera turn speed (deg/s) where it starts
    std::atomic<float> full_speed{150.0f};   // ... and where it is at full strength
};

Hud& hud();
Comfort& comfort();

void load();
void save_hud();
void save_comfort();
} // namespace config
