#include "game/census.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <MinHook.h>

#include "log.hpp"

namespace game::census {
namespace {
// ID3D11DeviceContext vtable slots.
constexpr size_t k_draw_indexed = 12;
constexpr size_t k_draw = 13;
constexpr size_t k_map = 14;
constexpr size_t k_depth = 10; // return addresses kept per call path

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
DrawIndexedFn g_draw_indexed = nullptr;
DrawFn g_draw = nullptr;
MapFn g_map = nullptr;
bool g_hooked = false;

enum class State { idle, armed, recording };
State g_state = State::idle; // render thread
bool g_key_was_down = false;

struct Path {
    uint32_t count = 0;
    uint32_t order = 0; // first occurrence within the frame
    DWORD thread = 0;
};
std::mutex g_mutex;
std::map<std::string, Path> g_paths;
uint32_t g_calls = 0;

void record(const char* kind) {
    uintptr_t frames[k_depth];
    const auto count = logger::scan_return_addresses(frames, k_depth, 8192);
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::string key = kind;
    for (size_t i = 0; i < count; ++i) {
        char entry[16];
        std::snprintf(entry, sizeof(entry), " %llx", (unsigned long long)(frames[i] - exe));
        key += entry;
    }

    std::scoped_lock _{g_mutex};
    auto& path = g_paths[key];
    if (path.count++ == 0) {
        path.order = g_calls;
        path.thread = GetCurrentThreadId();
    }
    ++g_calls;
}

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base) {
    if (g_state == State::recording) {
        record("DrawIndexed");
    }
    g_draw_indexed(self, count, start, base);
}

void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT count, UINT start) {
    if (g_state == State::recording) {
        record("Draw");
    }
    g_draw(self, count, start);
}

HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                              UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    if (g_state == State::recording) {
        record("Map");
    }
    return g_map(self, resource, subresource, type, flags, mapped);
}

bool install(ID3D11DeviceContext* context) {
    const auto vtable = *reinterpret_cast<void***>(context);
    if (MH_CreateHook(vtable[k_draw_indexed], reinterpret_cast<void*>(&draw_indexed),
                      reinterpret_cast<void**>(&g_draw_indexed)) != MH_OK ||
        MH_CreateHook(vtable[k_draw], reinterpret_cast<void*>(&draw), reinterpret_cast<void**>(&g_draw)) != MH_OK ||
        MH_CreateHook(vtable[k_map], reinterpret_cast<void*>(&map), reinterpret_cast<void**>(&g_map)) != MH_OK) {
        logger::error("[census] could not hook the context");
        return false;
    }
    MH_EnableHook(vtable[k_draw_indexed]);
    MH_EnableHook(vtable[k_draw]);
    MH_EnableHook(vtable[k_map]);
    return true;
}

void report() {
    std::vector<std::pair<std::string, Path>> rows;
    {
        std::scoped_lock _{g_mutex};
        rows.assign(g_paths.begin(), g_paths.end());
        g_paths.clear();
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.order < b.second.order; });

    logger::info("[census] one frame: %u calls, %zu distinct call paths (in order of first use; "
                 "return addresses, innermost first)", g_calls, rows.size());
    for (const auto& [key, path] : rows) {
        logger::info("[census] #%-5u x%-4u t%-5lu %s", path.order, path.count, path.thread, key.c_str());
    }
    g_calls = 0;
}
} // namespace

void on_present(ID3D11DeviceContext* context) {
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool pressed = down && !g_key_was_down;
    g_key_was_down = down;

    switch (g_state) {
    case State::idle:
        if (pressed && (g_hooked || (g_hooked = install(context)))) {
            g_state = State::armed;
            logger::backtrace("census: Present");
        }
        break;
    case State::armed:
        g_state = State::recording; // record from this Present to the next
        break;
    case State::recording:
        g_state = State::idle;
        report();
        break;
    }
}
} // namespace game::census
