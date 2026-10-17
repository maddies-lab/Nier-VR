#include "log.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <windows.h>

namespace logger {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
} // namespace

void open() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring log_path{path};
    log_path = log_path.substr(0, log_path.find_last_of(L"\\/") + 1) + L"NieR-VR.log";
    g_file = _wfopen(log_path.c_str(), L"w");
}

namespace {
uintptr_t g_text_start = 0, g_text_end = 0;

void find_text() {
    if (g_text_start != 0) {
        return;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (std::memcmp(section->Name, ".text", 5) == 0) {
            g_text_start = base + section->VirtualAddress;
            g_text_end = g_text_start + section->Misc.VirtualSize;
        }
    }
}

// True if the bytes before `address` end with a call instruction.
bool follows_call(uintptr_t address) {
    const auto b = reinterpret_cast<const uint8_t*>(address);
    if (address - 7 < g_text_start) {
        return false;
    }
    if (b[-5] == 0xE8) return true;                                               // call rel32
    if (b[-6] == 0xFF && (b[-5] == 0x15 || (b[-5] & 0xF8) == 0x90)) return true;   // call [rip+d32] / [reg+d32]
    if (b[-2] == 0xFF && (b[-1] & 0xF8) == 0xD0) return true;                      // call reg
    if (b[-3] == 0xFF && (b[-2] & 0xF8) == 0x50) return true;                      // call [reg+d8]
    if (b[-3] == 0x41 && b[-2] == 0xFF && (b[-1] & 0xF8) == 0xD0) return true;     // call r8-r15
    if (b[-7] == 0x41 && b[-6] == 0xFF && (b[-5] & 0xF8) == 0x90) return true;     // call [r8-r15+d32]
    if (b[-4] == 0x41 && b[-3] == 0xFF && (b[-2] & 0xF8) == 0x50) return true;     // call [r8-r15+d8]
    if (b[-3] == 0xFF && b[-2] == 0x14) return true;                               // call [reg+reg*s]
    return false;
}
} // namespace

size_t scan_return_addresses(uintptr_t* out, size_t max, size_t scan_bytes) {
    find_text();
    const auto tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto top = reinterpret_cast<uintptr_t>(tib->StackBase);
    auto p = reinterpret_cast<const uintptr_t*>(__builtin_frame_address(0));
    const auto end = reinterpret_cast<const uintptr_t*>(
        std::min<uintptr_t>(top, reinterpret_cast<uintptr_t>(p) + scan_bytes));
    size_t count = 0;
    for (; p < end && count < max; ++p) {
        const auto value = *p;
        if (value > g_text_start + 7 && value < g_text_end && follows_call(value)) {
            out[count++] = value;
        }
    }
    return count;
}

void backtrace(const char* label) {
    uintptr_t frames[32];
    const auto count = scan_return_addresses(frames, 32);
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string line;
    for (size_t i = 0; i < count; ++i) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), " %llx", (unsigned long long)(frames[i] - exe));
        line += entry;
    }
    write("info", std::string{"[stack] "} + label + " (thread " + std::to_string(GetCurrentThreadId()) + "):" + line);
}

void write(const char* level, const std::string& message) {
    std::scoped_lock _{g_mutex};
    if (g_file == nullptr) {
        return;
    }

    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, level, message.c_str());
    std::fflush(g_file);
}
} // namespace logger
