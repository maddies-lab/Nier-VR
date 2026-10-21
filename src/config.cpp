#include "config.hpp"

#include <cstdio>
#include <cwchar>
#include <string>

#include <windows.h>

#include "log.hpp"

namespace config {
namespace {
Hud g_hud;
Comfort g_comfort;

std::wstring path() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p{exe};
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"NieR-VR.ini";
}

float read(const wchar_t* section, const wchar_t* key, float fallback, float low, float high) {
    wchar_t buffer[32]{};
    GetPrivateProfileStringW(section, key, L"", buffer, 32, path().c_str());
    if (buffer[0] == 0) {
        return fallback;
    }
    const float value = std::wcstof(buffer, nullptr);
    return value >= low && value <= high ? value : fallback;
}

void write(const wchar_t* section, const wchar_t* key, float value) {
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.2f", value);
    WritePrivateProfileStringW(section, key, buffer, path().c_str());
}
} // namespace

Hud& hud() {
    return g_hud;
}

Comfort& comfort() {
    return g_comfort;
}

void load() {
    g_hud.width = read(L"hud", L"width", g_hud.width, 0.3f, 6.0f);
    g_hud.distance = read(L"hud", L"distance", g_hud.distance, 0.5f, 6.0f);
    g_hud.drop = read(L"hud", L"drop", g_hud.drop, -2.0f, 2.0f);
    g_hud.follow_degrees = read(L"hud", L"follow_degrees", g_hud.follow_degrees, 0.0f, 180.0f);
    logger::info("[config] hud width %.2f m, distance %.2f m, drop %.2f m, follow after %.0f deg",
                 g_hud.width.load(), g_hud.distance.load(), g_hud.drop.load(), g_hud.follow_degrees.load());
    g_comfort.vignette = read(L"comfort", L"vignette", g_comfort.vignette ? 1.0f : 0.0f, 0.0f, 1.0f) >= 0.5f;
    g_comfort.strength = read(L"comfort", L"vignette_strength", g_comfort.strength, 0.0f, 1.0f);
    g_comfort.start_speed = read(L"comfort", L"vignette_start_deg_per_s", g_comfort.start_speed, 0.0f, 720.0f);
    g_comfort.full_speed = read(L"comfort", L"vignette_full_deg_per_s", g_comfort.full_speed, 1.0f, 1440.0f);
    if (g_comfort.full_speed <= g_comfort.start_speed) {
        g_comfort.full_speed = g_comfort.start_speed + 1.0f;
    }
    logger::info("[config] comfort vignette %s, strength %.2f, %.0f..%.0f deg/s", g_comfort.vignette ? "on" : "off",
                 g_comfort.strength.load(), g_comfort.start_speed.load(), g_comfort.full_speed.load());
    save_hud(); // writes defaults for keys the file lacks, so they can be edited
    save_comfort();
}

void save_comfort() {
    write(L"comfort", L"vignette", g_comfort.vignette ? 1.0f : 0.0f);
    write(L"comfort", L"vignette_strength", g_comfort.strength);
    write(L"comfort", L"vignette_start_deg_per_s", g_comfort.start_speed);
    write(L"comfort", L"vignette_full_deg_per_s", g_comfort.full_speed);
}

void save_hud() {
    write(L"hud", L"width", g_hud.width);
    write(L"hud", L"distance", g_hud.distance);
    write(L"hud", L"drop", g_hud.drop);
    write(L"hud", L"follow_degrees", g_hud.follow_degrees);
}
} // namespace config
