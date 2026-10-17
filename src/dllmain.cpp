#include <windows.h>

#include <MinHook.h>

#include "game/build.hpp"
#include "game/camera.hpp"
#include "hooks/d3d11_hook.hpp"
#include "log.hpp"
#include "overlay.hpp"
#include "proxy/proxy.hpp"

namespace {
constexpr uint32_t k_camera_timeout_ms = 120'000;

DWORD WINAPI init_thread(LPVOID) {
    const auto build = game::identify_build();
    if (build.name != nullptr) {
        logger::info("build: %s", build.name);
    } else {
        logger::warn("build: unknown (stamp 0x%08X, image 0x%X); relying on scans only", build.timestamp,
                     build.size_of_image);
    }

    if (MH_Initialize() != MH_OK) {
        logger::error("MH_Initialize failed");
        return 1;
    }

    // Waits for SteamStub to decrypt .text and for the game's static objects to exist.
    if (game::CameraGame::locate(k_camera_timeout_ms) == nullptr) {
        logger::error("camera not found; staying inert");
        return 1;
    }

    hooks::install_d3d11({
        .on_present = overlay::on_present,
        .on_pre_resize = overlay::on_pre_resize,
    });
    return 0;
}
} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        logger::open();
        logger::info("NieR-VR loaded");

        if (!proxy::load_system_dll()) {
            MessageBoxW(nullptr, L"NieR-VR could not load the system DLL it proxies.", L"NieR-VR", MB_ICONERROR);
            return FALSE;
        }

        if (const auto thread = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
