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
