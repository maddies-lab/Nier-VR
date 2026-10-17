#pragma once

#include <cstddef>
#include <cstdint>

// Layouts from praydog/AutomataMP (MIT), shared/sdk/regenny.
namespace game {
struct Vec3 {
    float x, y, z;
};

struct Vec4 {
    float x, y, z, w;
};

struct Mat4 {
    Vec4 rows[4];
};

struct CameraData {
    char pad_0[0x80];
    Mat4 viewproj; // 0x80
    Mat4 view;     // 0xC0: camera-to-world, rows = right, up, forward, origin
};
static_assert(offsetof(CameraData, view) == 0xC0);

// cCameraGame : cCameraApp : cCamera : cCameraBase : CameraTransform
class CameraGame {
public:
    // Locates the static cCameraGame instance. Blocks until SteamStub has decrypted
    // .text and the object is constructed, or until `timeout_ms` elapses.
    static CameraGame* locate(uint32_t timeout_ms);

    // Last result of locate(), or nullptr.
    static CameraGame* get();

    static uintptr_t vtable();

    bool valid() const { return *reinterpret_cast<const uintptr_t*>(this) == vtable(); }

    const Vec3& position() const { return field<Vec3>(0x10); }
    CameraData* data() const { return field<CameraData*>(0x60); }
    float fov() const { return field<float>(0x138); } // vertical, radians

private:
    template <typename T>
    const T& field(uintptr_t offset) const {
        return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(this) + offset);
    }
};
} // namespace game
