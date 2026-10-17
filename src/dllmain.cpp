#include <string>

#include <windows.h>

#include <MinHook.h>

#include "game/build.hpp"
#include "game/camera.hpp"
#include "game/camera_hook.hpp"
#include "game/dump.hpp"
#include "game/stereo_render.hpp"
#include "hooks/d3d11_hook.hpp"
#include "log.hpp"
#include "overlay.hpp"
#include "proxy/proxy.hpp"
#include "xr/runtime.hpp"

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

    // NIERVR_DUMP=1 writes the decrypted executable next to the game for static analysis.
    wchar_t dump_flag[8]{};
    if (GetEnvironmentVariableW(L"NIERVR_DUMP", dump_flag, 8) > 0 && dump_flag[0] == L'1') {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring out{path};
        out = out.substr(0, out.find_last_of(L"\\/") + 1) + L"NieRAutomata.decrypted.exe";
        game::dump_executable(out);
    }

    game::camera_hook::install();
    game::stereo_render::install();

    hooks::install_d3d11({
        // The overlay draws first so it is visible on the virtual screen too.
        .on_present =
            [](IDXGISwapChain* swapchain) {
                overlay::on_present(swapchain);
                xr::on_present(swapchain);
            },
        .on_pre_resize = overlay::on_pre_resize,
    });
    return 0;
}
} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        // Other processes in the Proton prefix (e.g. its steam.exe helper) can pick up
        // this DLL too; they only get the forwarding.
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const std::wstring exe{exe_path};
        const auto name = exe.substr(exe.find_last_of(L"\\/") + 1);
        if (_wcsicmp(name.c_str(), L"NieRAutomata.exe") != 0) {
            return proxy::load_system_dll() ? TRUE : FALSE;
        }

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
