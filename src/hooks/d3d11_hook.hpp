#pragma once

#include <functional>

#include <dxgi.h>

namespace hooks {
struct D3D11Callbacks {
    // Called before the game's frame is presented.
    std::function<void(IDXGISwapChain*)> on_present;
    // Called before the swapchain buffers are resized; release backbuffer views here.
    std::function<void(IDXGISwapChain*)> on_pre_resize;
};

// Hooks IDXGISwapChain::Present and ResizeBuffers through a throwaway swapchain's vtable.
bool install_d3d11(D3D11Callbacks callbacks);
} // namespace hooks
