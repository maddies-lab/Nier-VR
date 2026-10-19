#pragma once

// Reverse-engineering helpers written to NieR-VR.log.
namespace game {
// F9: three presses - two in normal play, one on a screen to identify (e.g. game
// over). Logs the cCameraGame / CameraData fields that stayed the same in the first
// two and changed in the third.
void log_camera_snapshot();
} // namespace game
