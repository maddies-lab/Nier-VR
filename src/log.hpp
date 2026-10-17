#pragma once

#include <cstdio>
#include <string>

namespace logger {
// Opens NieR-VR.log next to the game executable. Safe to call once from DllMain.
void open();

void write(const char* level, const std::string& message);

template <typename... Args>
std::string format(const char* fmt, Args... args) {
    const auto size = std::snprintf(nullptr, 0, fmt, args...);
    std::string out(size > 0 ? size : 0, '\0');
    std::snprintf(out.data(), out.size() + 1, fmt, args...);
    return out;
}

template <typename... Args>
void info(const char* fmt, Args... args) {
    write("info", format(fmt, args...));
}

template <typename... Args>
void warn(const char* fmt, Args... args) {
    write("warn", format(fmt, args...));
}

template <typename... Args>
void error(const char* fmt, Args... args) {
    write("error", format(fmt, args...));
}
} // namespace logger
