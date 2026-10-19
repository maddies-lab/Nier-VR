#include "game/stereo_render.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

#include <MinHook.h>

#include "game/camera.hpp"
#include "hooks/context_hook.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "xr/runtime.hpp"

namespace game::stereo_render {
namespace {
constexpr UINT k_max_scanned_buffer = 1024;

using DrawJobFn = void(__fastcall*)(void* device);
DrawJobFn g_draw_job = nullptr;

// Constant-buffer uploads that carry the main camera, found by content while the
// main thread uploads the frame (before the draw job): the main SceneBuffer
// (view, projection, view*projection, camera-to-world), the buffer holding the
// previous frame's view*projection (motion vectors / motion-stretched effects) and
// the one holding the camera position (view-dependent shading). Before the right
// eye's pass each is re-uploaded with the right eye's values substituted.
enum class What : uint8_t { view, view_projection, camera_to_world, previous_view_projection, position };
const char* const k_what_names[] = {"view", "view*projection", "camera-to-world", "previous view*projection",
                                    "camera position"};

struct Field {
    UINT offset;
    What what;
    bool transposed;
};

struct Upload {
    ID3D11Resource* resource = nullptr;
    std::vector<uint8_t> data;
    std::vector<Field> fields;
};

std::mutex g_uploads_mutex;
std::vector<Upload> g_uploads; // this frame's camera uploads, one per buffer (cleared at Present)
constexpr size_t k_max_camera_uploads = 32;

// Cost diagnostics (per frame, logged every 300 frames).
uint64_t g_scan_ticks = 0, g_scanned = 0, g_matched = 0, g_patch_ticks = 0, g_frames = 0, g_over_cap = 0;

struct PendingMap {
    ID3D11Resource* resource = nullptr;
    const uint8_t* data = nullptr;
    UINT size = 0;
};
thread_local PendingMap t_pending;
bool g_registered = false;

// Logged once per (size, offset, kind) so the log shows which buffers are patched.
std::vector<uint64_t> g_seen_fields;

// The right eye's previous view*projection, for its motion vectors.
float g_previous_right_view_projection[16]{};
bool g_have_previous_right = false;

bool close_to(float a, float b) {
    return std::fabs(a - b) <= 1e-3f * std::fmax(1.0f, std::fabs(b));
}

// 16 floats at p equal m (as stored) or its transpose? Returns 0 no, 1 same, 2 transposed.
int match_matrix(const float* p, const float* m) {
    bool same = true, transposed = true;
    for (int r = 0; r < 4 && (same || transposed); ++r) {
        for (int c = 0; c < 4; ++c) {
            same = same && close_to(p[r * 4 + c], m[r * 4 + c]);
            transposed = transposed && close_to(p[r * 4 + c], m[c * 4 + r]);
        }
    }
    return same ? 1 : transposed ? 2 : 0;
}

std::vector<Field> find_camera_fields(const float* f, UINT size) {
    std::vector<Field> fields;
    const auto camera = CameraGame::get();
    if (camera == nullptr || camera->data() == nullptr) {
        return fields;
    }
    const auto data = reinterpret_cast<const float*>(camera->data());
    const float* matrices[] = {data + 0x00 / 4, data + 0x80 / 4, data + 0xC0 / 4, data + 0x200 / 4};
    const float* position = data + 0xF0 / 4;
    const UINT count = size / 4;

    std::vector<bool> covered(count, false);
    for (UINT i = 0; i + 16 <= count; ++i) {
        for (int w = 0; w < 4; ++w) {
            if (const int how = match_matrix(f + i, matrices[w])) {
                fields.push_back({i * 4, static_cast<What>(w), how == 2});
                std::fill(covered.begin() + i, covered.begin() + i + 16, true);
                i += 15;
                break;
            }
        }
    }
    const bool position_is_distinctive =
        std::fabs(position[0]) + std::fabs(position[1]) + std::fabs(position[2]) > 1.0f;
    for (UINT i = 0; position_is_distinctive && i + 3 <= count; ++i) {
        if (!covered[i] && close_to(f[i], position[0]) && close_to(f[i + 1], position[1]) &&
            close_to(f[i + 2], position[2])) {
            fields.push_back({i * 4, What::position, false});
            i += 2;
        }
    }
    return fields;
}

void on_map(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT hr) {
    t_pending = {};
    if (FAILED(hr) || type != D3D11_MAP_WRITE_DISCARD || !xr::both_eyes_active()) {
        return;
    }
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (desc.ByteWidth >= 12 && desc.ByteWidth <= k_max_scanned_buffer &&
        (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) {
        t_pending = {resource, static_cast<const uint8_t*>(mapped->pData), desc.ByteWidth};
    }
}

void on_unmap(ID3D11Resource* resource) {
    if (resource != t_pending.resource || t_pending.data == nullptr) {
        return;
    }
    const auto pending = t_pending;
    t_pending = {};

    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
    auto fields = find_camera_fields(reinterpret_cast<const float*>(pending.data), pending.size);
    QueryPerformanceCounter(&after);
    g_scan_ticks += after.QuadPart - before.QuadPart;
    ++g_scanned;
    if (fields.empty()) {
        return;
    }
    ++g_matched;
    for (const auto& field : fields) {
        const uint64_t key = (uint64_t(pending.size) << 32) | (uint64_t(field.offset) << 8) | uint64_t(field.what);
        if (std::find(g_seen_fields.begin(), g_seen_fields.end(), key) == g_seen_fields.end() &&
            g_seen_fields.size() < 64) {
            g_seen_fields.push_back(key);
            logger::info("[stereo] camera upload: %u-byte buffer +0x%x = %s%s", pending.size, field.offset,
                         k_what_names[static_cast<int>(field.what)], field.transposed ? " (transposed)" : "");
        }
    }

    Upload upload{resource, std::vector<uint8_t>(pending.data, pending.data + pending.size), std::move(fields)};
    std::scoped_lock _{g_uploads_mutex};
    // Draws only ever see a buffer's last upload of the frame.
    for (auto& existing : g_uploads) {
        if (existing.resource == resource) {
            existing = std::move(upload);
            return;
        }
    }
    g_uploads.push_back(std::move(upload));
}

// Re-uploads this frame's camera uploads with the right eye's values.
bool upload_right_eye(ID3D11DeviceContext* context, const float (&scene)[64]) {
    // The SceneBuffer block holds the right eye's matrices transposed.
    float right[4][16];
    for (int k = 0; k < 3; ++k) {
        const int block = k == 0 ? 0 : k == 1 ? 2 : 3; // view, view*projection, camera-to-world
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                right[k][r * 4 + c] = scene[block * 16 + c * 4 + r];
            }
        }
    }
    std::memcpy(right[3], g_have_previous_right ? g_previous_right_view_projection : right[1], sizeof(right[3]));
    const float position[3] = {right[2][12], right[2][13], right[2][14]};

    std::scoped_lock _{g_uploads_mutex};
    if (g_uploads.empty()) {
        return false;
    }
    if (g_uploads.size() > k_max_camera_uploads) {
        if (g_over_cap++ == 0) {
            logger::warn("[stereo] %zu camera uploads in one frame; drawing one eye for such frames", g_uploads.size());
        }
        return false;
    }
    LARGE_INTEGER before, after;
    QueryPerformanceCounter(&before);
    for (const auto& upload : g_uploads) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(hooks::context::map_original(context, upload.resource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            continue;
        }
        auto out = static_cast<uint8_t*>(mapped.pData);
        std::memcpy(out, upload.data.data(), upload.data.size());
        for (const auto& field : upload.fields) {
            auto f = reinterpret_cast<float*>(out + field.offset);
            if (field.what == What::position) {
                std::memcpy(f, position, sizeof(position));
                continue;
            }
            const float* m = right[static_cast<int>(field.what)];
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    f[r * 4 + c] = field.transposed ? m[c * 4 + r] : m[r * 4 + c];
                }
            }
        }
        hooks::context::unmap_original(context, upload.resource, 0);
    }

    QueryPerformanceCounter(&after);
    g_patch_ticks += after.QuadPart - before.QuadPart;

    std::memcpy(g_previous_right_view_projection, right[1], sizeof(right[1]));
    g_have_previous_right = true;
    return true;
}

std::atomic<uint64_t> g_doubled{0}, g_single{0};

// Per-pass timing, logged every 300 doubled frames: CPU time of each job run, the
// part of it spent waiting in Map(READ), and GPU time from timestamp queries.
struct PassTimes {
    double cpu_ms = 0, read_wait_ms = 0, gpu_ms = 0;
    uint32_t gpu_samples = 0;
};
PassTimes g_left, g_right;
double g_handoff_gpu_ms = 0; // left image copy to the headset (includes waiting on the XR side's fence)
uint32_t g_timed_frames = 0;

constexpr int k_query_ring = 4;
struct GpuQueries {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamps[4]{}; // start, after left, after the left hand-off, after right
    bool pending = false;
};
GpuQueries g_queries[k_query_ring];
int g_query_index = 0;
bool g_queries_ready = false;

double qpc_ms(int64_t ticks) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return ticks * 1000.0 / frequency.QuadPart;
}

void create_queries(ID3D11DeviceContext* context) {
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    bool ok = device != nullptr;
    for (auto& q : g_queries) {
        ok = ok && SUCCEEDED(device->CreateQuery(&disjoint, &q.disjoint));
        for (auto& s : q.stamps) {
            ok = ok && SUCCEEDED(device->CreateQuery(&stamp, &s));
        }
    }
    if (device != nullptr) {
        device->Release();
    }
    g_queries_ready = ok;
    if (!ok) {
        logger::warn("[stereo] GPU timestamp queries unavailable");
    }
}

// Reads finished query sets without waiting.
void collect_gpu_times(ID3D11DeviceContext* context) {
    for (auto& q : g_queries) {
        if (!q.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 stamps[4]{};
        if (context->GetData(q.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        bool ready = true;
        for (int i = 0; i < 4 && ready; ++i) {
            ready = context->GetData(q.stamps[i], &stamps[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }
        if (!ready) {
            continue;
        }
        q.pending = false;
        if (!disjoint.Disjoint && disjoint.Frequency != 0) {
            g_left.gpu_ms += (stamps[1] - stamps[0]) * 1000.0 / disjoint.Frequency;
            g_handoff_gpu_ms += (stamps[2] - stamps[1]) * 1000.0 / disjoint.Frequency;
            g_right.gpu_ms += (stamps[3] - stamps[2]) * 1000.0 / disjoint.Frequency;
            ++g_left.gpu_samples;
            ++g_right.gpu_samples;
        }
    }
}

void report_times() {
    if (++g_timed_frames < 300) {
        return;
    }
    const auto avg = [](double total, uint32_t n) { return n > 0 ? total / n : 0.0; };
    logger::info("[stereo] per frame: left eye cpu %.2f ms gpu %.2f ms | left hand-off gpu %.2f ms | right eye cpu "
                 "%.2f ms gpu %.2f ms (read-back waits %.2f / %.2f ms)",
                 g_left.cpu_ms / g_timed_frames, avg(g_left.gpu_ms, g_left.gpu_samples),
                 avg(g_handoff_gpu_ms, g_left.gpu_samples), g_right.cpu_ms / g_timed_frames,
                 avg(g_right.gpu_ms, g_right.gpu_samples), g_left.read_wait_ms / g_timed_frames,
                 g_right.read_wait_ms / g_timed_frames);
    g_handoff_gpu_ms = 0;
    g_left = {};
    g_right = {};
    g_timed_frames = 0;
}

void __fastcall draw_job_detour(void* device) {
    xr::JobViews views;
    const auto context = xr::game_context();
    bool have_uploads;
    {
        std::scoped_lock _{g_uploads_mutex};
        have_uploads = !g_uploads.empty();
    }
    if (!have_uploads || context == nullptr || !xr::job_views(views)) {
        g_have_previous_right = false;
        ++g_single;
        g_draw_job(device);
        return;
    }

    if (!g_queries_ready) {
        create_queries(context);
    }
    collect_gpu_times(context);
    auto& queries = g_queries[g_query_index];
    const bool timing = g_queries_ready && !queries.pending;
    if (timing) {
        context->Begin(queries.disjoint);
        context->End(queries.stamps[0]);
    }

    // Left eye: the queue as recorded and uploaded.
    LARGE_INTEGER t0, t1, t2;
    uint64_t wait0 = hooks::context::read_map_ticks();
    QueryPerformanceCounter(&t0);
    g_draw_job(device);
    QueryPerformanceCounter(&t1);
    const uint64_t wait1 = hooks::context::read_map_ticks();
    if (timing) {
        context->End(queries.stamps[1]);
    }
    xr::publish_from_job(views.primary, views.sequence);
    if (timing) {
        context->End(queries.stamps[2]);
    }

    // Right eye: same queue, the camera uploads redone with the right eye.
    if (!upload_right_eye(context, views.secondary_scene)) {
        return;
    }
    const uint64_t wait2 = hooks::context::read_map_ticks();
    LARGE_INTEGER t3;
    QueryPerformanceCounter(&t2);
    g_draw_job(device);
    QueryPerformanceCounter(&t3);
    const uint64_t wait3 = hooks::context::read_map_ticks();
    if (timing) {
        context->End(queries.stamps[3]);
        context->End(queries.disjoint);
        queries.pending = true;
        g_query_index = (g_query_index + 1) % k_query_ring;
    }
    xr::mark_both_rendered(views.sequence);

    g_left.cpu_ms += qpc_ms(t1.QuadPart - t0.QuadPart);
    g_left.read_wait_ms += qpc_ms(static_cast<int64_t>(wait1 - wait0));
    g_right.cpu_ms += qpc_ms(t3.QuadPart - t2.QuadPart);
    g_right.read_wait_ms += qpc_ms(static_cast<int64_t>(wait3 - wait2));
    report_times();

    if (++g_doubled == 1) {
        logger::info("[stereo] first frame drawn for both eyes");
    }
}
} // namespace

bool install() {
    const auto exe = GetModuleHandleW(nullptr);
    const auto text = scan::section(exe, ".text");
    if (!text) {
        return false;
    }

    // The draw job: begin, execute the render queue, end.
    const auto target = scan::find_pattern(text->start, text->size,
                                           "40 53 48 83 EC 20 48 8B D9 B9 01 00 00 00 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? "
                                           "?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 48 8D 0D");
    if (!target) {
        logger::error("[stereo] draw job not found; both-eyes rendering unavailable");
        return false;
    }

    const auto address = reinterpret_cast<void*>(*target);
    if (MH_CreateHook(address, reinterpret_cast<void*>(&draw_job_detour), reinterpret_cast<void**>(&g_draw_job)) !=
            MH_OK ||
        MH_EnableHook(address) != MH_OK) {
        logger::error("[stereo] could not hook the draw job");
        return false;
    }
    logger::info("[stereo] draw job hooked at exe+0x%llx",
                 (unsigned long long)(*target - reinterpret_cast<uintptr_t>(exe)));
    return true;
}

void on_present(ID3D11DeviceContext* context) {
    if (!g_registered && context != nullptr && hooks::context::install(context)) {
        hooks::context::add_observer({on_map, on_unmap, nullptr});
        g_registered = true;
    }
    // The next frame's uploads start after this Present.
    std::scoped_lock _{g_uploads_mutex};
    if (!g_uploads.empty() && ++g_frames % 300 == 0) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double ms = 1000.0 / frequency.QuadPart / 300.0;
        logger::info("[stereo] camera uploads per frame: %.1f buffers scanned (%.2f ms), %.1f matched, %zu kept, "
                     "right-eye re-upload %.2f ms",
                     g_scanned / 300.0, g_scan_ticks * ms, g_matched / 300.0, g_uploads.size(), g_patch_ticks * ms);
        g_scanned = g_matched = g_scan_ticks = g_patch_ticks = 0;
    }
    g_uploads.clear();
}
} // namespace game::stereo_render
