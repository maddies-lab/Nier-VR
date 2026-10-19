#include "game/inspect.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include "game/camera.hpp"
#include "log.hpp"

namespace game {
namespace {
constexpr size_t k_camera_size = 0x8C20;
constexpr size_t k_data_size = 0x2A0;

struct Snapshot {
    std::vector<uint8_t> camera, data;
};
std::vector<Snapshot> g_snapshots;

// 32-bit fields equal in the first two snapshots (taken during normal play) that
// differ in the third (taken on the screen to tell apart).
void report(const char* label, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
            const std::vector<uint8_t>& c) {
    int shown = 0;
    for (size_t offset = 0; offset + 4 <= a.size() && shown < 150; offset += 4) {
        uint32_t x, y, z;
        std::memcpy(&x, &a[offset], 4);
        std::memcpy(&y, &b[offset], 4);
        std::memcpy(&z, &c[offset], 4);
        if (x == y && y != z) {
            float fy, fz;
            std::memcpy(&fy, &y, 4);
            std::memcpy(&fz, &z, 4);
            logger::info("[snap] %s+0x%04zx: %08x -> %08x  (int %d -> %d, float %.4g -> %.4g)", label, offset, y, z,
                         (int)y, (int)z, fy, fz);
            ++shown;
        }
    }
    logger::info("[snap] %s: %d candidate fields%s", label, shown, shown == 150 ? " (truncated)" : "");
}
} // namespace

void log_camera_snapshot() {
    const auto camera = CameraGame::get();
    if (camera == nullptr || !camera->valid() || camera->data() == nullptr) {
        logger::warn("[snap] no valid cCameraGame");
        return;
    }

    const auto cam = reinterpret_cast<const uint8_t*>(camera);
    const auto data = reinterpret_cast<const uint8_t*>(camera->data());
    g_snapshots.push_back({std::vector<uint8_t>(cam, cam + k_camera_size),
                           std::vector<uint8_t>(data, data + k_data_size)});
    logger::info("[snap] snapshot %zu of 3 taken%s", g_snapshots.size(),
                 g_snapshots.size() < 3 ? " (1 and 2 in normal play, 3 on the screen to identify)" : "");
    if (g_snapshots.size() < 3) {
        return;
    }

    report("cCameraGame", g_snapshots[0].camera, g_snapshots[1].camera, g_snapshots[2].camera);
    report("CameraData", g_snapshots[0].data, g_snapshots[1].data, g_snapshots[2].data);
    g_snapshots.clear();
}
} // namespace game
