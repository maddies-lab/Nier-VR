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
constexpr int k_channels = 2; // 0 = left eye / mono / theater, 1 = right eye

// How the XR side should present a frame.
struct FrameMeta {
    bool projected = false;                    // false: theater quad; true: projection layer
    int eye = -1;                              // -1 mono/theater, 0 left, 1 right
    bool paired = false;                       // half of a both-eyes pair (see claim_pair)
    uint64_t sequence = 0;                     // camera update the frame was rendered from
    float orientation[4]{0, 0, 0, 1};          // head pose the frame was rendered for (LOCAL space)
    float position[3]{};
    float tan_left = 0, tan_right = 0, tan_up = 0, tan_down = 0;
};

struct Frame {
    int slot = -1;
    uint64_t generation = 0;
    uint64_t game_value = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    FrameMeta meta;
    int channel = 0;
};

// Game side (the game's immediate context). Copies the backbuffer into a free slot
// of the channel and publishes it.
bool publish(int channel, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
             const FrameMeta& meta);

// XR thread. Claims the channel's newest frame if it has not been consumed yet.
std::optional<Frame> claim_newest(int channel, ID3D11Device* device);

// XR thread. Claims a both-eyes pair: the newest left and right images, only when
// both belong to the same game frame (so the eyes never see different moments).
bool claim_pair(ID3D11Device* device, Frame& left, Frame& right);

// Diagnostics: sequences of each channel's newest frame (0 if none) and whether
// it is half of a pair.
struct NewestInfo {
    uint64_t sequence = 0;
    bool paired = false;
    bool fresh = false; // not yet taken by the XR side
};
NewestInfo newest_info(int channel);

// XR thread. Copies a claimed frame into `destination` and releases the claim.
void copy_and_release(const Frame& frame, ID3D11DeviceContext* context, ID3D11Texture2D* destination);

// XR thread. Releases a claim without copying (e.g. the swapchain image was not ready).
void release(const Frame& frame);

// "shared textures + fences", "shared textures, flush only" or "-".
const char* description();
} // namespace xr::transport
