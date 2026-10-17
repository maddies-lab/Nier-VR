#include "game/stereo_render.hpp"

#include <atomic>
#include <cmath>
#include <cstring>

#include <windows.h>

#include <MinHook.h>

#include "game/camera.hpp"
#include "hooks/context_hook.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "xr/runtime.hpp"

namespace game::stereo_render {
namespace {
constexpr UINT k_scene_buffer_size = 256;

using DrawJobFn = void(__fastcall*)(void* device);
DrawJobFn g_draw_job = nullptr;

// The main camera's SceneBuffer: the 256-byte constant buffer whose upload equals
// CameraData+0x00..0xFF transposed. Found again whenever the upload stops matching.
std::atomic<ID3D11Resource*> g_scene_buffer{nullptr};
thread_local ID3D11Resource* t_mapped_resource = nullptr;
thread_local const float* t_mapped_data = nullptr;
bool g_registered = false;

// Uploaded ViewProj (SceneBuffer +0x80, transposed) == CameraData+0x80?
bool matches_main_camera(const float* scene) {
    const auto camera = CameraGame::get();
    if (camera == nullptr || camera->data() == nullptr) {
        return false;
    }
    const auto view_projection = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(camera->data()) + 0x80);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float uploaded = scene[32 + r * 4 + c];
            const float expected = view_projection[c * 4 + r];
            if (std::fabs(uploaded - expected) > 1e-3f * std::fmax(1.0f, std::fabs(expected))) {
                return false;
            }
        }
    }
    return true;
}

void on_map(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped, HRESULT hr) {
    t_mapped_resource = nullptr;
    if (FAILED(hr) || type != D3D11_MAP_WRITE_DISCARD) {
        return;
    }
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return;
    }
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (desc.ByteWidth == k_scene_buffer_size && (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) {
        t_mapped_resource = resource;
        t_mapped_data = static_cast<const float*>(mapped->pData);
    }
}

void on_unmap(ID3D11Resource* resource) {
    if (resource != t_mapped_resource || t_mapped_data == nullptr) {
        return;
    }
    t_mapped_resource = nullptr;
    if (matches_main_camera(t_mapped_data) && g_scene_buffer.exchange(resource) != resource) {
        logger::info("[stereo] main camera SceneBuffer is %p", (void*)resource);
    }
}

std::atomic<uint64_t> g_doubled{0}, g_single{0};

// Per-pass timing, logged every 300 doubled frames: CPU time of each job run, the
// part of it spent waiting in Map(READ), and GPU time from timestamp queries.
struct PassTimes {
    double cpu_ms = 0, read_wait_ms = 0, gpu_ms = 0;
    uint32_t gpu_samples = 0;
};
PassTimes g_left, g_right;
uint32_t g_timed_frames = 0;

constexpr int k_query_ring = 4;
struct GpuQueries {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamps[3]{}; // start, after left, after right
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
        UINT64 stamps[3]{};
        if (context->GetData(q.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        bool ready = true;
        for (int i = 0; i < 3 && ready; ++i) {
            ready = context->GetData(q.stamps[i], &stamps[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }
        if (!ready) {
            continue;
        }
        q.pending = false;
        if (!disjoint.Disjoint && disjoint.Frequency != 0) {
            g_left.gpu_ms += (stamps[1] - stamps[0]) * 1000.0 / disjoint.Frequency;
            g_right.gpu_ms += (stamps[2] - stamps[1]) * 1000.0 / disjoint.Frequency;
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
    logger::info("[stereo] per frame: left eye cpu %.2f ms (read-back wait %.2f) gpu %.2f ms | right eye cpu %.2f ms "
                 "(read-back wait %.2f) gpu %.2f ms",
                 g_left.cpu_ms / g_timed_frames, g_left.read_wait_ms / g_timed_frames,
                 avg(g_left.gpu_ms, g_left.gpu_samples), g_right.cpu_ms / g_timed_frames,
                 g_right.read_wait_ms / g_timed_frames, avg(g_right.gpu_ms, g_right.gpu_samples));
    g_left = {};
    g_right = {};
    g_timed_frames = 0;
}

void __fastcall draw_job_detour(void* device) {
    xr::JobViews views;
    const auto scene_buffer = g_scene_buffer.load();
    const auto context = xr::game_context();
    if (scene_buffer == nullptr || context == nullptr || !xr::job_views(views)) {
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

    // Right eye: same queue, the main camera's SceneBuffer re-uploaded.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(hooks::context::map_original(context, scene_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, views.secondary_scene, sizeof(views.secondary_scene));
    hooks::context::unmap_original(context, scene_buffer, 0);
    const uint64_t wait2 = hooks::context::read_map_ticks();
    LARGE_INTEGER t3;
    QueryPerformanceCounter(&t2);
    g_draw_job(device);
    QueryPerformanceCounter(&t3);
    const uint64_t wait3 = hooks::context::read_map_ticks();
    if (timing) {
        context->End(queries.stamps[2]);
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
}
} // namespace game::stereo_render
