#include "game/camera.hpp"

#include <atomic>
#include <optional>

#include <windows.h>

#include "log.hpp"
#include "scan.hpp"

namespace game {
namespace {
std::atomic<CameraGame*> g_camera{nullptr};
uintptr_t g_vtable{};

// AutomataMP's method: the only code referencing "Step : %d / %d" loads the
// cCameraGame instance with `lea rcx, [rip+disp]` shortly afterwards.
std::optional<uintptr_t> find_by_string_reference(HMODULE exe, scan::Range text, scan::Range rdata) {
    const auto str = scan::find_string(rdata, "Step : %d / %d");
    if (!str) {
        return std::nullopt;
    }

    const auto ref = scan::find_rip_reference(text, *str);
    if (!ref) {
        return std::nullopt; // .text still encrypted, or the code moved
    }

    const auto lea = scan::find_pattern(*ref + 4, 100, "48 8D 0D ?? ?? ?? ??");
    if (!lea) {
        return std::nullopt;
    }

    return scan::resolve_rel32(*lea + 3);
}

// Fallback: the static instance is the only 8-byte-aligned qword in .data
// holding the cCameraGame vftable.
std::optional<uintptr_t> find_by_vtable_value(scan::Range data) {
    const auto words = reinterpret_cast<const uintptr_t*>(data.start);
    for (size_t i = 0; i < data.size / sizeof(uintptr_t); ++i) {
        if (words[i] == g_vtable) {
            return data.start + i * sizeof(uintptr_t);
        }
    }

    return std::nullopt;
}
} // namespace

uintptr_t CameraGame::vtable() {
    return g_vtable;
}

CameraGame* CameraGame::get() {
    return g_camera.load();
}

CameraGame* CameraGame::locate(uint32_t timeout_ms) {
    const auto exe = GetModuleHandleW(nullptr);
    const auto text = scan::section(exe, ".text");
    const auto rdata = scan::section(exe, ".rdata");
    const auto data = scan::section(exe, ".data");
    if (!text || !rdata || !data) {
        logger::error("[camera] executable sections not found");
        return nullptr;
    }

    // RTTI lives in .data/.rdata, which SteamStub leaves unencrypted.
    if (const auto vt = scan::find_vtable(exe, ".?AVcCameraGame@@")) {
        g_vtable = *vt;
        logger::info("[camera] cCameraGame vftable at exe+0x%llx", (unsigned long long)(*vt - (uintptr_t)exe));
    } else {
        logger::error("[camera] cCameraGame vftable not found (unknown build?)");
        return nullptr;
    }

    const auto start = GetTickCount64();
    bool logged_string_miss = false;
    while (GetTickCount64() - start < timeout_ms) {
        std::optional<uintptr_t> candidate = find_by_string_reference(exe, *text, *rdata);
        const char* method = "string reference";
        if (candidate && !data->contains(*candidate)) {
            candidate.reset(); // static instance must live in .data/.bss
        }
        if (!candidate || *reinterpret_cast<const uintptr_t*>(*candidate) != g_vtable) {
            if (candidate && !logged_string_miss) {
                logger::warn("[camera] string-reference candidate exe+0x%llx has wrong vftable yet",
                             (unsigned long long)(*candidate - (uintptr_t)exe));
                logged_string_miss = true;
            }
            candidate = find_by_vtable_value(*data);
            method = "vftable value";
        }

        if (candidate && *reinterpret_cast<const uintptr_t*>(*candidate) == g_vtable) {
            const auto camera = reinterpret_cast<CameraGame*>(*candidate);
            g_camera = camera;
            logger::info("[camera] cCameraGame instance at exe+0x%llx (via %s) after %llu ms",
                         (unsigned long long)(*candidate - (uintptr_t)exe), method,
                         (unsigned long long)(GetTickCount64() - start));
            return camera;
        }

        Sleep(250);
    }

    logger::error("[camera] cCameraGame instance not found within %u ms", timeout_ms);
    return nullptr;
}
} // namespace game
