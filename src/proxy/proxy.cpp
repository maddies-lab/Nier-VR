#include "proxy/proxy.hpp"

#include <cstdint>
#include <string>

#include <windows.h>

// Target for exports the system DLL does not provide (e.g. Wine-only names).
extern "C" uintptr_t proxy_missing_export() {
    return 0;
}

namespace proxy {
bool load_system_dll() {
    wchar_t dir[MAX_PATH]{};
    if (GetSystemDirectoryW(dir, MAX_PATH) == 0) {
        return false;
    }

    const auto path = std::wstring{dir} + L"\\" + k_system_dll;
    const auto module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        return false;
    }

    for (size_t i = 0; i < k_export_count; ++i) {
        const auto& e = k_exports[i];
        const auto key = e.name != nullptr ? e.name : MAKEINTRESOURCEA(e.ordinal);
        const auto target = reinterpret_cast<void*>(GetProcAddress(module, key));
        k_slots[i] = target != nullptr ? target : reinterpret_cast<void*>(&proxy_missing_export);
    }

    return true;
}
} // namespace proxy
