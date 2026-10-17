#pragma once

#include <cstdint>
#include <optional>

#include <d3d11.h>

// Moves finished game frames from the game's D3D11 device to the XR device.
//
// A ring of three shared textures plus two shared fences, with every wait on the
// GPU, so the game thread never blocks on the XR side:
//   game: [GPU wait xr_fence >= slot's last read]  copy backbuffer -> slot, signal game_fence
//   XR:   [GPU wait game_fence >= slot's write]    copy slot -> destination, signal xr_fence
// The game writes any slot that is neither the newest frame nor the one the XR
// thread has claimed, so a free slot always exists.
//
// Without shared-fence support it degrades to flushes only (rare tearing possible).
// (DXVK's keyed mutex was tried first: its ReleaseSync makes the CPU wait for the
// GPU to finish the game's whole frame, up to 60 ms in heavy scenes.)
namespace xr::transport {
struct Frame {
    int slot = -1;
    uint64_t generation = 0;
    uint64_t game_value = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Render thread. Copies the backbuffer into a free slot and publishes it.
bool publish(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer);

// XR thread. Claims the newest frame if it has not been consumed yet.
std::optional<Frame> claim_newest(ID3D11Device* device);

// XR thread. Copies a claimed frame into `destination` and releases the claim.
void copy_and_release(const Frame& frame, ID3D11DeviceContext* context, ID3D11Texture2D* destination);

// XR thread. Releases a claim without copying (e.g. the swapchain image was not ready).
void release(const Frame& frame);

// "shared textures + fences", "shared textures, flush only" or "-".
const char* description();
} // namespace xr::transport
