#include "game/inspect.hpp"

#include <cstdint>
#include <cstring>

#include "game/camera.hpp"
#include "log.hpp"

namespace game {
namespace {
void dump_rows(const char* label, const void* base, size_t size) {
    const auto bytes = static_cast<const uint8_t*>(base);
    for (size_t offset = 0; offset < size; offset += 16) {
        float f[4];
        uint64_t q[2];
        std::memcpy(f, bytes + offset, sizeof(f));
        std::memcpy(q, bytes + offset, sizeof(q));
        logger::info("[snap] %s+%03zx  %12.4f %12.4f %12.4f %12.4f   %016llx %016llx", label, offset, f[0], f[1], f[2],
                     f[3], (unsigned long long)q[0], (unsigned long long)q[1]);
    }
}
} // namespace

void log_camera_snapshot() {
    static int index = 0;
    const auto camera = CameraGame::get();
    if (camera == nullptr || !camera->valid()) {
        logger::warn("[snap] no valid cCameraGame");
        return;
    }

    logger::info("[snap] ---- snapshot %d: cCameraGame %p, CameraData %p", ++index, (void*)camera,
                 (void*)camera->data());
    dump_rows("cam", camera, 0x400);
    if (camera->data() != nullptr) {
        dump_rows("data", camera->data(), 0x400);
    }
}
} // namespace game
