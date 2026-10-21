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

Hud& hud();

void load();
void save_hud();
} // namespace config
