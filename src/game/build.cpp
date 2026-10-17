#include "game/build.hpp"

#include <windows.h>

namespace game {
namespace {
struct KnownBuild {
    uint32_t timestamp;
    uint32_t size_of_image;
    const char* name;
};

// Steam build from the July 2021 patch; both executables ship side by side.
constexpr KnownBuild k_known_builds[] = {
    {0x60D3A8D2, 0x18F7000, "Steam 2021-07 (Win10)"},
    {0x60D3A8BF, 0x18E9000, "Steam 2021-07 (Compat, Win7/8)"},
};
} // namespace

BuildInfo identify_build() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    BuildInfo info{nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nullptr};
    for (const auto& known : k_known_builds) {
        if (known.timestamp == info.timestamp && known.size_of_image == info.size_of_image) {
            info.name = known.name;
        }
    }

    return info;
}
} // namespace game
