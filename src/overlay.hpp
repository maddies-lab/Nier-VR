#pragma once

#include <dxgi.h>

// Desktop debug overlay (ImGui on the game's backbuffer). Insert toggles it.
namespace overlay {
void on_present(IDXGISwapChain* swapchain);
void on_pre_resize(IDXGISwapChain* swapchain);
} // namespace overlay
