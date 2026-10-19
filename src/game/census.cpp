#include "game/census.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "game/camera.hpp"
#include "hooks/context_hook.hpp"
#include "log.hpp"

namespace game::census {
namespace {
constexpr size_t k_depth = 10; // return addresses kept per call path

enum class State { idle, armed, recording };
State g_state = State::idle; // render thread
bool g_key_was_down = false;
bool g_registered = false;

struct Path {
    uint32_t count = 0;
    uint32_t order = 0; // first occurrence within the frame
    DWORD thread = 0;
};
std::mutex g_mutex;
std::map<std::string, Path> g_paths;
uint32_t g_calls = 0;

// Constant buffers mapped during the recording: scanned at Unmap for copies of the
// main camera (both-eyes mode needs every camera-dependent upload re-done for the
// right eye); 256 B / 32 B ones are also dumped.
struct PendingMap {
    ID3D11Resource* resource = nullptr;
    const void* data = nullptr;
    UINT size = 0;
    std::string path;
};
thread_local PendingMap t_pending;
int g_dumps = 0;
int g_structured_dumps = 0;

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

void on_draw(const char* kind) {
    if (g_state == State::recording) {
        record(kind);
    }
}

void on_map(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT hr) {
    if (g_state != State::recording) {
        return;
    }

    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    UINT size = 0;
    D3D11_BUFFER_DESC desc{};
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        size = desc.ByteWidth;
    }
    char kind[80];
    if ((desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0) {
        std::snprintf(kind, sizeof(kind), "Map[structured %u stride %u bind 0x%x t%d]", size,
                      desc.StructureByteStride, desc.BindFlags, (int)type);
    } else {
        std::snprintf(kind, sizeof(kind), "Map[%s %u bind 0x%x t%d]",
                      dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? "buf" : "tex", size, desc.BindFlags, (int)type);
    }
    record(kind);

    // Light (48-byte) and probe (96-byte) arrays: dump the first elements so their
    // layout can be checked against the camera (both-eyes lighting).
    if (SUCCEEDED(hr) && (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0 &&
        (desc.StructureByteStride == 48 || desc.StructureByteStride == 96) && g_structured_dumps < 6) {
        ++g_structured_dumps;
        float local[6 * 24];
        const UINT elements = std::min<UINT>(6, size / desc.StructureByteStride);
        std::memcpy(local, mapped->pData, elements * desc.StructureByteStride);
        const UINT floats = desc.StructureByteStride / 4;
        logger::info("[census] structured %u-byte buffer %p, stride %u (%u elements), first %u:", size,
                     (void*)resource, desc.StructureByteStride, size / desc.StructureByteStride, elements);
        for (UINT e = 0; e < elements; ++e) {
            std::string line;
            for (UINT k = 0; k < floats; ++k) {
                char v[16];
                std::snprintf(v, sizeof(v), " %.3f", local[e * floats + k]);
                line += v;
            }
            logger::info("[census]   [%u]%s", e, line.c_str());
        }
    }
    if (SUCCEEDED(hr) && dimension == D3D11_RESOURCE_DIMENSION_BUFFER && size >= 12 && size <= 65536) {
        uintptr_t frames[6];
        const auto count = logger::scan_return_addresses(frames, 6, 8192);
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::string path;
        for (size_t i = 0; i < count; ++i) {
            char entry[16];
            std::snprintf(entry, sizeof(entry), " %llx", (unsigned long long)(frames[i] - exe));
            path += entry;
        }
        t_pending = {resource, mapped->pData, size, path};
    }
}

bool close_to(float a, float b) {
    return std::fabs(a - b) <= 1e-3f * std::fmax(1.0f, std::fabs(b));
}

// 16 floats at p equal matrix m (row-major as stored) or its transpose?
const char* match_matrix(const float* p, const float* m) {
    bool same = true, transposed = true;
    for (int r = 0; r < 4 && (same || transposed); ++r) {
        for (int c = 0; c < 4; ++c) {
            same = same && close_to(p[r * 4 + c], m[r * 4 + c]);
            transposed = transposed && close_to(p[r * 4 + c], m[c * 4 + r]);
        }
    }
    return same ? "" : transposed ? " (transposed)" : nullptr;
}

int g_camera_hits = 0;

void scan_for_camera(const PendingMap& pending) {
    const auto camera = CameraGame::get();
    if (camera == nullptr || camera->data() == nullptr || g_camera_hits >= 80) {
        return;
    }
    const auto data = reinterpret_cast<const float*>(camera->data());
    struct Candidate {
        const char* name;
        const float* value;
    };
    const Candidate matrices[] = {{"view", data + 0x00 / 4},
                                  {"projection", data + 0x40 / 4},
                                  {"view*projection", data + 0x80 / 4},
                                  {"camera-to-world", data + 0xC0 / 4},
                                  {"previous view*projection", data + 0x200 / 4}};
    const float* position = data + 0xF0 / 4; // camera-to-world origin = eye position
    const auto f = static_cast<const float*>(pending.data);
    const UINT count = pending.size / 4;
    for (UINT i = 0; i + 3 <= count; ++i) {
        for (const auto& candidate : matrices) {
            if (i + 16 <= count) {
                if (const char* how = match_matrix(f + i, candidate.value)) {
                    logger::info("[census] camera data: %u-byte buffer %p +0x%x = %s%s  (path%s)", pending.size,
                                 (void*)pending.resource, i * 4, candidate.name, how, pending.path.c_str());
                    ++g_camera_hits;
                }
            }
        }
        if (close_to(f[i], position[0]) && close_to(f[i + 1], position[1]) && close_to(f[i + 2], position[2]) &&
            std::fabs(position[0]) + std::fabs(position[1]) + std::fabs(position[2]) > 1.0f) {
            logger::info("[census] camera data: %u-byte buffer %p +0x%x = camera position  (path%s)", pending.size,
                         (void*)pending.resource, i * 4, pending.path.c_str());
            ++g_camera_hits;
        }
    }
}

void on_unmap(ID3D11Resource* resource) {
    if (g_state == State::recording && t_pending.resource == resource) {
        scan_for_camera(t_pending);
    }
    if (g_state == State::recording && t_pending.resource == resource && g_dumps < 24 &&
        (t_pending.size == 256 || t_pending.size == 32)) {
        ++g_dumps;
        const auto f = static_cast<const float*>(t_pending.data);
        logger::info("[census] %u-byte buffer %p contents:", t_pending.size, (void*)resource);
        for (UINT row = 0; row < t_pending.size / 16; ++row) {
            logger::info("[census]   +%03x %10.4f %10.4f %10.4f %10.4f", row * 16, f[row * 4], f[row * 4 + 1],
                         f[row * 4 + 2], f[row * 4 + 3]);
        }
        t_pending = {};
    }
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
    g_dumps = 0;
    g_camera_hits = 0;
    g_structured_dumps = 0;
}
} // namespace

void on_present(ID3D11DeviceContext* context) {
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool pressed = down && !g_key_was_down;
    g_key_was_down = down;

    switch (g_state) {
    case State::idle:
        if (pressed && hooks::context::install(context)) {
            if (!g_registered) {
                hooks::context::add_observer({on_map, on_unmap, on_draw});
                g_registered = true;
            }
            g_state = State::armed;
            if (const auto camera = CameraGame::get(); camera != nullptr && camera->data() != nullptr) {
                const auto m = reinterpret_cast<const float*>(camera->data());
                for (int r = 0; r < 4; ++r) {
                    logger::info("[census] left camera view row %d: %.4f %.4f %.4f %.4f   camera-to-world row %d: "
                                 "%.4f %.4f %.4f %.4f",
                                 r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3], r, m[48 + r * 4],
                                 m[48 + r * 4 + 1], m[48 + r * 4 + 2], m[48 + r * 4 + 3]);
                }
            }
            logger::backtrace("census: Present");
        }
        break;
    case State::armed:
        hooks::context::set_draw_observation(true);
        g_state = State::recording; // record from this Present to the next
        break;
    case State::recording:
        hooks::context::set_draw_observation(false);
        g_state = State::idle;
        report();
        break;
    }
}
} // namespace game::census
