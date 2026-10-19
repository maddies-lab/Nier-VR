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
// Sizes of the constant buffers found carrying the main camera (F11 census): the
// main SceneBuffer (256), previous view*projection (80) and camera position (32).
// Only these are scanned: the game maps ~2000 constant buffers per frame, and
// reading mapped upload memory from the CPU is slow (write-combined), so a scan of
// all of them cost ~500 ms per frame.
constexpr UINT k_scanned_sizes[] = {32, 80, 256};
constexpr UINT k_max_scanned_buffer = 256;

using DrawJobFn = void(__fastcall*)(void* device);
DrawJobFn g_draw_job = nullptr;

// The render-queue executor the draw job calls (exe+0x247D20 in the Compat build):
// walks the queue's command list and calls each node's callback.
//   node: +0x00 callback, +0x08 argument, +0x18 next, +0x20 sort key
//   callback(device, &key_bytes, argument)
using ExecuteFn = void(__fastcall*)(void* queue, void* device);
ExecuteFn g_execute = nullptr;
using CommandFn = void(__fastcall*)(void* device, void* key, void* argument);

// F10 node census: for one both-eyes frame the executor is replicated in both
// passes, recording each node's callback and how many draws it produced, to find
// the commands that behave differently the second time (the right pass draws
// fewer calls than the left even with the same camera).
struct NodeRecord {
    uintptr_t callback;
    uintptr_t argument;
    uint32_t key;
    uint32_t draws;
    uint32_t queries;
};
std::atomic<int> g_census_pass{-1}; // -1 off, 0 left pass, 1 right pass
bool g_census_armed = false, g_census_key_was_down = false;
std::vector<NodeRecord> g_census_nodes[2];

void __fastcall execute_detour(void* queue, void* device) {
    const int pass = g_census_pass;
    if (pass < 0) {
        g_execute(queue, device);
        return;
    }
    auto& records = g_census_nodes[pass];
    const auto q = static_cast<uint8_t*>(queue);
    if (*reinterpret_cast<int*>(q + 0xA4) == 0) {
        return;
    }
    auto node = *reinterpret_cast<uintptr_t**>(q + 0x68);
    while (node != nullptr) {
        const auto next = reinterpret_cast<uintptr_t*>(node[3]);
        if (node[0] != 0) {
            const uint32_t sort = static_cast<uint32_t>(node[4]);
            struct {
                uint8_t b0, b1, pad[2];
                uint32_t low;
            } key{static_cast<uint8_t>(sort >> 30), static_cast<uint8_t>((sort >> 23) & 0x7F), {0, 0}, sort & 0xFFFFF};
            const auto draws_before = hooks::context::counter(hooks::context::count_draw);
            const auto queries_before = hooks::context::counter(hooks::context::count_query_begin);
            reinterpret_cast<CommandFn>(node[0])(device, &key, reinterpret_cast<void*>(node[1]));
            records.push_back({node[0], node[1], sort,
                               static_cast<uint32_t>(hooks::context::counter(hooks::context::count_draw) - draws_before),
                               static_cast<uint32_t>(hooks::context::counter(hooks::context::count_query_begin) -
                                                     queries_before)});
        }
        node = next;
    }
}

void report_node_census() {
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto& l = g_census_nodes[0];
    const auto& r = g_census_nodes[1];
    uint64_t ld = 0, rd = 0;
    for (const auto& n : l) ld += n.draws;
    for (const auto& n : r) rd += n.draws;
    logger::info("[nodes] left pass: %zu commands, %llu draws; right pass: %zu commands, %llu draws", l.size(),
                 (unsigned long long)ld, r.size(), (unsigned long long)rd);

    // Draws per callback in each pass, and the callbacks that differ.
    struct PerCallback {
        uint64_t calls[2]{}, draws[2]{}, queries[2]{};
    };
    std::vector<std::pair<uintptr_t, PerCallback>> per;
    auto add = [&](int pass, const NodeRecord& n) {
        auto it = std::find_if(per.begin(), per.end(), [&](const auto& e) { return e.first == n.callback; });
        if (it == per.end()) {
            per.push_back({n.callback, {}});
            it = per.end() - 1;
        }
        ++it->second.calls[pass];
        it->second.draws[pass] += n.draws;
        it->second.queries[pass] += n.queries;
    };
    for (const auto& n : l) add(0, n);
    for (const auto& n : r) add(1, n);
    for (const auto& [callback, c] : per) {
        const bool differs = c.calls[0] != c.calls[1] || c.draws[0] != c.draws[1] || c.queries[0] != c.queries[1];
        logger::info("[nodes] %s callback exe+0x%llx: commands %llu | %llu, draws %llu | %llu, queries %llu | %llu",
                     differs ? "DIFFERS" : "same   ", (unsigned long long)(callback - exe),
                     (unsigned long long)c.calls[0], (unsigned long long)c.calls[1], (unsigned long long)c.draws[0],
                     (unsigned long long)c.draws[1], (unsigned long long)c.queries[0], (unsigned long long)c.queries[1]);
    }
    // The first individual commands that differ, in order.
    int shown = 0;
    for (size_t i = 0; i < std::min(l.size(), r.size()) && shown < 40; ++i) {
        if (l[i].callback != r[i].callback || l[i].argument != r[i].argument || l[i].draws != r[i].draws) {
            logger::info("[nodes] #%zu left exe+0x%llx arg %llx key %08x draws %u | right exe+0x%llx arg %llx key %08x "
                         "draws %u",
                         i, (unsigned long long)(l[i].callback - exe), (unsigned long long)l[i].argument, l[i].key,
                         l[i].draws, (unsigned long long)(r[i].callback - exe), (unsigned long long)r[i].argument,
                         r[i].key, r[i].draws);
            ++shown;
        }
    }
    g_census_nodes[0].clear();
    g_census_nodes[1].clear();
}

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

// F4 (diagnostic): the right pass reuses the left eye's uploads, i.e. the same
// camera drawn twice. If the eyes still differ, the difference comes from work
// that changes state each time the draw job runs, not from camera data.
std::atomic<bool> g_same_camera_test{false};
bool g_test_key_was_down = false;

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
    if (std::find(std::begin(k_scanned_sizes), std::end(k_scanned_sizes), desc.ByteWidth) !=
            std::end(k_scanned_sizes) &&
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
    // One sequential read of the mapped memory; the scan works on the copy.
    alignas(16) uint8_t local[k_max_scanned_buffer];
    std::memcpy(local, pending.data, pending.size);
    auto fields = find_camera_fields(reinterpret_cast<const float*>(local), pending.size);
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

    Upload upload{resource, std::vector<uint8_t>(local, local + pending.size), std::move(fields)};
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

// GPU state the draw job carries across frames (read-modify-write: eye adaptation,
// GPU particles, history buffers) must be the same at the start of both passes,
// or the right eye sees it one step further. Resources the job writes through
// UAVs, and small render targets, are learned during the left pass; from the next
// frame on they are snapshotted before the left pass and restored before the right.
// Full-size render targets are left out: the job redraws them every pass.
constexpr UINT k_small_target_pixels = 512 * 512;
constexpr uint64_t k_forget_after_frames = 300;
constexpr size_t k_max_tracked = 256;

struct Tracked {
    ID3D11Resource* resource = nullptr; // AddRef'd
    ID3D11Resource* backup = nullptr;
    uint64_t last_written = 0;          // frame the left pass last wrote it
    uint64_t bytes = 0;
};
std::vector<Tracked> g_tracked;         // draw-job thread only
std::vector<ID3D11Resource*> g_written; // this left pass (collected by the binding hooks)
std::mutex g_written_mutex;
uint64_t g_frame_index = 0;
bool g_restore_enabled = true;          // F3 toggles (A/B test)
bool g_restore_large = false;           // F2: also full-size render targets (previous-frame history)
bool g_large_key_was_down = false;

// Per-pass call counts, compared between the passes (F4 test: should match).
struct PassCounts {
    uint64_t c[hooks::context::count_kinds]{};
};
PassCounts g_count_sum[2];
uint32_t g_count_frames = 0, g_count_mismatches = 0;

PassCounts read_counts() {
    PassCounts p;
    for (int i = 0; i < hooks::context::count_kinds; ++i) {
        p.c[i] = hooks::context::counter(static_cast<hooks::context::Counter>(i));
    }
    return p;
}

void add_pass_counts(int pass, const PassCounts& before, const PassCounts& after) {
    bool same = true;
    static PassCounts left;
    for (int i = 0; i < hooks::context::count_kinds; ++i) {
        const uint64_t n = after.c[i] - before.c[i];
        g_count_sum[pass].c[i] += n;
        if (pass == 0) {
            left.c[i] = n;
        } else {
            same = same && left.c[i] == n;
        }
    }
    if (pass == 1) {
        g_count_mismatches += same ? 0 : 1;
        if (++g_count_frames == 300) {
            const auto& l = g_count_sum[0].c;
            const auto& r = g_count_sum[1].c;
            logger::info("[stereo] per pass (avg of 300 frames, left | right): draws %.1f | %.1f, indirect draws "
                         "%.1f | %.1f, dispatches %.1f | %.1f, predication %.1f | %.1f, queries %.1f | %.1f; "
                         "%u frames with different counts",
                         l[0] / 300.0, r[0] / 300.0, l[1] / 300.0, r[1] / 300.0, l[2] / 300.0, r[2] / 300.0,
                         l[3] / 300.0, r[3] / 300.0, l[4] / 300.0, r[4] / 300.0, g_count_mismatches);
            g_count_sum[0] = {};
            g_count_sum[1] = {};
            g_count_frames = 0;
            g_count_mismatches = 0;
        }
    }
}
bool g_restore_key_was_down = false;

// Bytes per pixel of the formats we care to size; others count as 8.
uint64_t texture_bytes(const D3D11_TEXTURE2D_DESC& d) {
    return uint64_t(d.Width) * d.Height * std::max<UINT>(1, d.ArraySize) * 8;
}

void on_write_views(ID3D11View* const* views, UINT count) {
    std::scoped_lock _{g_written_mutex};
    for (UINT i = 0; i < count; ++i) {
        if (views[i] == nullptr) {
            continue;
        }
        ID3D11Resource* resource = nullptr;
        views[i]->GetResource(&resource);
        if (resource == nullptr) {
            continue;
        }

        // UAVs: always. Render targets: only small ones.
        ID3D11UnorderedAccessView* uav = nullptr;
        const bool is_uav =
            SUCCEEDED(views[i]->QueryInterface(__uuidof(ID3D11UnorderedAccessView), reinterpret_cast<void**>(&uav)));
        if (uav != nullptr) {
            uav->Release();
        }
        bool keep = is_uav;
        if (!keep) {
            D3D11_RESOURCE_DIMENSION dimension{};
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC desc{};
                static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
                keep = g_restore_large || desc.Width * desc.Height <= k_small_target_pixels;
            }
        }
        if (keep && std::find(g_written.begin(), g_written.end(), resource) == g_written.end()) {
            g_written.push_back(resource);
        }
        resource->Release(); // the game owns it; Tracked AddRefs what it keeps
    }
}

ID3D11Resource* create_backup(ID3D11Device* device, ID3D11Resource* resource, uint64_t& bytes) {
    D3D11_RESOURCE_DIMENSION dimension{};
    resource->GetType(&dimension);
    ID3D11Resource* backup = nullptr;
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        bytes = desc.ByteWidth;
        device->CreateBuffer(&desc, nullptr, reinterpret_cast<ID3D11Buffer**>(&backup));
    } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        if (desc.SampleDesc.Count > 1) {
            return nullptr;
        }
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags &= ~(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                            D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
        bytes = texture_bytes(desc);
        device->CreateTexture2D(&desc, nullptr, reinterpret_cast<ID3D11Texture2D**>(&backup));
    } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC desc{};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags &= ~D3D11_RESOURCE_MISC_SHARED;
        bytes = uint64_t(desc.Width) * desc.Height * desc.Depth * 8;
        device->CreateTexture3D(&desc, nullptr, reinterpret_cast<ID3D11Texture3D**>(&backup));
    }
    return backup;
}

// After the left pass: adopt newly written resources, forget stale ones.
void update_tracked(ID3D11DeviceContext* context) {
    std::vector<ID3D11Resource*> written;
    {
        std::scoped_lock _{g_written_mutex};
        written.swap(g_written);
    }
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    bool changed = false;
    for (auto resource : written) {
        auto it = std::find_if(g_tracked.begin(), g_tracked.end(),
                               [&](const Tracked& t) { return t.resource == resource; });
        if (it != g_tracked.end()) {
            it->last_written = g_frame_index;
            continue;
        }
        if (g_tracked.size() >= k_max_tracked || device == nullptr) {
            continue;
        }
        uint64_t bytes = 0;
        if (auto backup = create_backup(device, resource, bytes)) {
            resource->AddRef();
            g_tracked.push_back({resource, backup, g_frame_index, bytes});
            changed = true;
        }
    }
    if (device != nullptr) {
        device->Release();
    }

    const auto before = g_tracked.size();
    g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
                                   [](Tracked& t) {
                                       if (g_frame_index - t.last_written <= k_forget_after_frames) {
                                           return false;
                                       }
                                       t.backup->Release();
                                       t.resource->Release();
                                       return true;
                                   }),
                    g_tracked.end());
    if (changed || g_tracked.size() != before) {
        uint64_t total = 0;
        for (const auto& t : g_tracked) {
            total += t.bytes;
        }
        logger::info("[stereo] per-pass GPU state: %zu resources tracked (~%.1f MB)", g_tracked.size(),
                     total / 1048576.0);
    }
}

void snapshot_state(ID3D11DeviceContext* context) {
    for (const auto& t : g_tracked) {
        context->CopyResource(t.backup, t.resource);
    }
}

void restore_state(ID3D11DeviceContext* context) {
    for (const auto& t : g_tracked) {
        context->CopyResource(t.resource, t.backup);
    }
}

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

    // Left eye: the queue as recorded and uploaded, from snapshotted GPU state.
    ++g_frame_index;
    if (g_restore_enabled) {
        snapshot_state(context);
    }
    LARGE_INTEGER t0, t1, t2;
    uint64_t wait0 = hooks::context::read_map_ticks();
    QueryPerformanceCounter(&t0);
    hooks::context::set_write_observation(true);
    const auto left_before = read_counts();
    const bool census = g_census_armed;
    g_census_armed = false;
    if (census) {
        g_census_pass = 0;
    }
    g_draw_job(device);
    add_pass_counts(0, left_before, read_counts());
    hooks::context::set_write_observation(false);
    QueryPerformanceCounter(&t1);
    const uint64_t wait1 = hooks::context::read_map_ticks();
    if (timing) {
        context->End(queries.stamps[1]);
    }
    xr::publish_from_job(views.primary, views.sequence);
    if (timing) {
        context->End(queries.stamps[2]);
    }

    // Right eye: same queue and the same starting GPU state, the camera uploads
    // redone with the right eye.
    if (g_restore_enabled) {
        restore_state(context);
    }
    update_tracked(context);
    if (!g_same_camera_test && !upload_right_eye(context, views.secondary_scene)) {
        return;
    }
    const uint64_t wait2 = hooks::context::read_map_ticks();
    LARGE_INTEGER t3;
    QueryPerformanceCounter(&t2);
    const auto right_before = read_counts();
    if (census) {
        g_census_pass = 1;
    }
    g_draw_job(device);
    g_census_pass = -1;
    if (census) {
        report_node_census();
    }
    add_pass_counts(1, right_before, read_counts());
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

    const auto executor = scan::find_pattern(
        text->start, text->size,
        "40 55 48 83 EC 20 83 B9 A4 00 00 00 00 48 8B EA 0F 84 ?? ?? ?? ?? 48 89 5C 24 38 48 8B 59 68");
    if (executor && MH_CreateHook(reinterpret_cast<void*>(*executor), reinterpret_cast<void*>(&execute_detour),
                                  reinterpret_cast<void**>(&g_execute)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(*executor)) == MH_OK) {
        logger::info("[stereo] render-queue executor hooked at exe+0x%llx",
                     (unsigned long long)(*executor - reinterpret_cast<uintptr_t>(exe)));
    } else {
        logger::warn("[stereo] render-queue executor not hooked; F10 node census unavailable");
    }
    return true;
}

void on_present(ID3D11DeviceContext* context) {
    if (!g_registered && context != nullptr && hooks::context::install(context)) {
        hooks::context::add_observer({on_map, on_unmap, nullptr, on_write_views});
        g_registered = true;
    }
    const bool test_down = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (test_down && !g_test_key_was_down) {
        g_same_camera_test = !g_same_camera_test;
        logger::info("[stereo] F4 test: right pass %s", g_same_camera_test ? "uses the LEFT camera (both eyes identical "
                                                                             "unless state advances per pass)"
                                                                           : "uses the right camera");
    }
    g_test_key_was_down = test_down;

    const bool restore_down = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
    if (restore_down && !g_restore_key_was_down) {
        g_restore_enabled = !g_restore_enabled;
        logger::info("[stereo] F3: per-pass GPU state restore %s", g_restore_enabled ? "on" : "off");
    }
    g_restore_key_was_down = restore_down;

    const bool large_down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (large_down && !g_large_key_was_down) {
        g_restore_large = !g_restore_large;
        logger::info("[stereo] F2: full-size render targets %s the per-pass restore",
                     g_restore_large ? "included in" : "excluded from");
    }
    g_large_key_was_down = large_down;

    const bool census_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (census_down && !g_census_key_was_down) {
        g_census_armed = true;
        logger::info("[nodes] F10: recording the next both-eyes frame's render commands");
    }
    g_census_key_was_down = census_down;

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
