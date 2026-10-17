#pragma once

// Reverse-engineering helpers: raw memory snapshots written to NieR-VR.log.
namespace game {
// Logs cCameraGame and its CameraData as rows of four floats (plus the raw
// qwords, so pointers stand out). Bound to F9 in the overlay.
void log_camera_snapshot();
} // namespace game
