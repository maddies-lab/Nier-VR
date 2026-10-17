#include "log.hpp"

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

void backtrace(const char* label) {
    void* frames[48]{};
    const auto count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string line;
    for (USHORT i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        char entry[40];
        if (address >= exe && address < exe + 0x2000000) {
            std::snprintf(entry, sizeof(entry), " exe+0x%llx", (unsigned long long)(address - exe));
        } else {
            std::snprintf(entry, sizeof(entry), " %p", frames[i]);
        }
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
