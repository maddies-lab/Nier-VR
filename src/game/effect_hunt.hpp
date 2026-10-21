#pragma once

#include <string>

#include <d3d11.h>

// Interactive tool for finding a full-screen pass (vignette, speed lines, ...):
// End records the next frame's full-screen draws (<= 6 vertices into a target at
// least 1280 wide) and logs them numbered by pixel shader; Page Down / Page Up
// switch one of them off at a time so the player can see which one it is.
namespace game::effect_hunt {
// Interceptor hook (any thread drawing on the game's context).
bool should_skip(ID3D11DeviceContext* context, UINT count);

// Render thread: keys, and the end of a recording.
void on_present();

// For the overlay: "" when idle.
std::string status();
} // namespace game::effect_hunt
