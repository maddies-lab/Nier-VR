#include "hooks/context_hook.hpp"

#include <atomic>

#include <MinHook.h>

#include "log.hpp"

namespace hooks::context {
namespace {
// ID3D11DeviceContext vtable slots.
constexpr size_t k_draw_indexed = 12;
constexpr size_t k_draw = 13;
constexpr size_t k_map = 14;
constexpr size_t k_unmap = 15;
constexpr size_t k_om_set_render_targets = 33;
constexpr size_t k_om_set_render_targets_and_uavs = 34;
constexpr size_t k_cs_set_uavs = 68;
constexpr size_t k_draw_indexed_instanced = 20;
constexpr size_t k_draw_instanced = 21;
constexpr size_t k_begin = 27;
constexpr size_t k_end = 28;
constexpr size_t k_set_predication = 30;
constexpr size_t k_draw_indexed_instanced_indirect = 39;
constexpr size_t k_draw_instanced_indirect = 40;
constexpr size_t k_dispatch = 41;
constexpr size_t k_dispatch_indirect = 42;
constexpr size_t k_copy_subresource_region = 46;
constexpr size_t k_copy_resource = 47;
constexpr size_t k_update_subresource = 48;

Observer g_observers[4];
std::atomic<int> g_observer_count{0};
std::atomic<uint64_t> g_counters[count_kinds];
DrawInterceptor g_interceptor;
std::atomic<bool> g_intercepting{false};

// Runs the interceptor around a draw; returns false if the draw is to be skipped.
struct InterceptScope {
    ID3D11DeviceContext* context;
    bool active;
    explicit InterceptScope(ID3D11DeviceContext* c) : context(c), active(g_intercepting) {}
    bool skip(UINT count) { return active && g_interceptor.before(context, count); }
    ~InterceptScope() {
        if (active && !skipped && g_interceptor.after) {
            g_interceptor.after(context);
        }
    }
    bool skipped = false;
};

using DIIFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DIFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using AsyncFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using PredFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Predicate*, BOOL);
using IndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
DIIFn g_dii = nullptr;
DIFn g_di = nullptr;
AsyncFn g_begin = nullptr, g_end = nullptr;
PredFn g_pred = nullptr;
IndirectFn g_dii_indirect = nullptr, g_di_indirect = nullptr, g_dispatch_indirect = nullptr;
DispatchFn g_dispatch = nullptr;
using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                              ID3D11Resource*, UINT, const D3D11_BOX*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                          const void*, UINT, UINT);
CopyRegionFn g_copy_region = nullptr;
CopyFn g_copy = nullptr;
UpdateFn g_update = nullptr;

void notify_draw_detail(ID3D11DeviceContext* c, int kind, UINT a, UINT b, UINT d, UINT e, UINT f);

void STDMETHODCALLTYPE copy_region(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z,
                                   ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
    ++g_counters[count_copy];
    g_copy_region(c, dst, ds, x, y, z, src, ss, box);
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    ++g_counters[count_copy];
    g_copy(c, dst, src);
}
void STDMETHODCALLTYPE update(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, const D3D11_BOX* box,
                              const void* data, UINT pitch, UINT depth_pitch) {
    ++g_counters[count_update_subresource];
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_update) {
            g_observers[i].on_update(dst, ds, box, data);
        }
    }
    g_update(c, dst, ds, box, data, pitch, depth_pitch);
}

void STDMETHODCALLTYPE dii(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    ++g_counters[count_draw];
    notify_draw_detail(c, 2, a, b, d, static_cast<UINT>(e), f);
    InterceptScope scope{c};
    if (scope.skip(a)) {
        scope.skipped = true;
        return;
    }
    g_dii(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE di(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    ++g_counters[count_draw];
    notify_draw_detail(c, 3, a, b, d, e, 0);
    InterceptScope scope{c};
    if (scope.skip(a)) {
        scope.skipped = true;
        return;
    }
    g_di(c, a, b, d, e);
}
void STDMETHODCALLTYPE begin(ID3D11DeviceContext* c, ID3D11Asynchronous* a) {
    ++g_counters[count_query_begin];
    g_begin(c, a);
}
void STDMETHODCALLTYPE end(ID3D11DeviceContext* c, ID3D11Asynchronous* a) {
    ++g_counters[count_query_end];
    g_end(c, a);
}
void STDMETHODCALLTYPE set_predication(ID3D11DeviceContext* c, ID3D11Predicate* p, BOOL v) {
    if (p != nullptr) {
        ++g_counters[count_predication];
    }
    g_pred(c, p, v);
}
void STDMETHODCALLTYPE dii_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    ++g_counters[count_draw_indirect];
    g_dii_indirect(c, b, o);
}
void STDMETHODCALLTYPE di_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    ++g_counters[count_draw_indirect];
    g_di_indirect(c, b, o);
}
void STDMETHODCALLTYPE dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    ++g_counters[count_dispatch];
    g_dispatch(c, x, y, z);
}
void STDMETHODCALLTYPE dispatch_indirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
    ++g_counters[count_dispatch];
    g_dispatch_indirect(c, b, o);
}

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
using OMSetRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*, UINT, UINT,
                                              ID3D11UnorderedAccessView* const*, const UINT*);
using CSSetUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                            const UINT*);
OMSetRTFn g_om_set_rt = nullptr;
OMSetRTUAVFn g_om_set_rt_uav = nullptr;
CSSetUAVFn g_cs_set_uav = nullptr;
std::atomic<bool> g_observe_writes{false};
DrawIndexedFn g_draw_indexed = nullptr;
DrawFn g_draw = nullptr;
MapFn g_map = nullptr;
UnmapFn g_unmap = nullptr;
std::atomic<bool> g_installed{false};

std::atomic<bool> g_observe_draws{false};
thread_local uint64_t t_read_map_ticks = 0;

void notify_draw(const char* kind) {
    if (!g_observe_draws) {
        return;
    }
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_draw) {
            g_observers[i].on_draw(kind);
        }
    }
}

void notify_draw_detail(ID3D11DeviceContext* c, int kind, UINT a, UINT b, UINT d, UINT e, UINT f) {
    if (!g_observe_draws) {
        return;
    }
    const UINT args[5] = {a, b, d, e, f};
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_draw_detail) {
            g_observers[i].on_draw_detail(c, kind, args);
        }
    }
}

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base) {
    ++g_counters[count_draw];
    notify_draw_detail(self, 0, count, start, static_cast<UINT>(base), 0, 0);
    InterceptScope scope{self};
    if (scope.skip(count)) {
        scope.skipped = true;
        return;
    }
    notify_draw("DrawIndexed");
    g_draw_indexed(self, count, start, base);
}

void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT count, UINT start) {
    ++g_counters[count_draw];
    notify_draw_detail(self, 1, count, start, 0, 0, 0);
    InterceptScope scope{self};
    if (scope.skip(count)) {
        scope.skipped = true;
        return;
    }
    notify_draw("Draw");
    g_draw(self, count, start);
}

HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                              UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    const bool read = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE;
    LARGE_INTEGER before{}, after{};
    if (read) {
        QueryPerformanceCounter(&before);
    }
    const auto hr = g_map(self, resource, subresource, type, flags, mapped);
    if (read) {
        QueryPerformanceCounter(&after);
        t_read_map_ticks += after.QuadPart - before.QuadPart;
    }
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_map) {
            g_observers[i].on_map(resource, type, mapped, hr);
        }
    }
    return hr;
}

void notify_writes(ID3D11View* const* views, UINT count) {
    if (!g_observe_writes || views == nullptr || count == 0) {
        return;
    }
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_write_views) {
            g_observers[i].on_write_views(views, count);
        }
    }
}

void STDMETHODCALLTYPE om_set_rt(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views,
                                 ID3D11DepthStencilView* depth) {
    notify_writes(reinterpret_cast<ID3D11View* const*>(views), count);
    g_om_set_rt(self, count, views, depth);
}

void STDMETHODCALLTYPE om_set_rt_uav(ID3D11DeviceContext* self, UINT count, ID3D11RenderTargetView* const* views,
                                     ID3D11DepthStencilView* depth, UINT uav_start, UINT uav_count,
                                     ID3D11UnorderedAccessView* const* uavs, const UINT* initial) {
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        notify_writes(reinterpret_cast<ID3D11View* const*>(views), count);
    }
    if (uav_count != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
        notify_writes(reinterpret_cast<ID3D11View* const*>(uavs), uav_count);
    }
    g_om_set_rt_uav(self, count, views, depth, uav_start, uav_count, uavs, initial);
}

void STDMETHODCALLTYPE cs_set_uav(ID3D11DeviceContext* self, UINT start, UINT count,
                                  ID3D11UnorderedAccessView* const* uavs, const UINT* initial) {
    notify_writes(reinterpret_cast<ID3D11View* const*>(uavs), count);
    g_cs_set_uav(self, start, count, uavs, initial);
}

void STDMETHODCALLTYPE unmap(ID3D11DeviceContext* self, ID3D11Resource* resource, UINT subresource) {
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_unmap) {
            g_observers[i].on_unmap(resource);
        }
    }
    g_unmap(self, resource, subresource);
}
} // namespace

bool install(ID3D11DeviceContext* context) {
    if (g_installed) {
        return true;
    }

    const auto vtable = *reinterpret_cast<void***>(context);
    if (MH_CreateHook(vtable[k_draw_indexed], reinterpret_cast<void*>(&draw_indexed),
                      reinterpret_cast<void**>(&g_draw_indexed)) != MH_OK ||
        MH_CreateHook(vtable[k_draw], reinterpret_cast<void*>(&draw), reinterpret_cast<void**>(&g_draw)) != MH_OK ||
        MH_CreateHook(vtable[k_map], reinterpret_cast<void*>(&map), reinterpret_cast<void**>(&g_map)) != MH_OK ||
        MH_CreateHook(vtable[k_unmap], reinterpret_cast<void*>(&unmap), reinterpret_cast<void**>(&g_unmap)) != MH_OK ||
        MH_CreateHook(vtable[k_om_set_render_targets], reinterpret_cast<void*>(&om_set_rt),
                      reinterpret_cast<void**>(&g_om_set_rt)) != MH_OK ||
        MH_CreateHook(vtable[k_om_set_render_targets_and_uavs], reinterpret_cast<void*>(&om_set_rt_uav),
                      reinterpret_cast<void**>(&g_om_set_rt_uav)) != MH_OK ||
        MH_CreateHook(vtable[k_cs_set_uavs], reinterpret_cast<void*>(&cs_set_uav),
                      reinterpret_cast<void**>(&g_cs_set_uav)) != MH_OK ||
        MH_CreateHook(vtable[k_draw_indexed_instanced], reinterpret_cast<void*>(&dii),
                      reinterpret_cast<void**>(&g_dii)) != MH_OK ||
        MH_CreateHook(vtable[k_draw_instanced], reinterpret_cast<void*>(&di), reinterpret_cast<void**>(&g_di)) !=
            MH_OK ||
        MH_CreateHook(vtable[k_begin], reinterpret_cast<void*>(&begin), reinterpret_cast<void**>(&g_begin)) != MH_OK ||
        MH_CreateHook(vtable[k_end], reinterpret_cast<void*>(&end), reinterpret_cast<void**>(&g_end)) != MH_OK ||
        MH_CreateHook(vtable[k_set_predication], reinterpret_cast<void*>(&set_predication),
                      reinterpret_cast<void**>(&g_pred)) != MH_OK ||
        MH_CreateHook(vtable[k_draw_indexed_instanced_indirect], reinterpret_cast<void*>(&dii_indirect),
                      reinterpret_cast<void**>(&g_dii_indirect)) != MH_OK ||
        MH_CreateHook(vtable[k_draw_instanced_indirect], reinterpret_cast<void*>(&di_indirect),
                      reinterpret_cast<void**>(&g_di_indirect)) != MH_OK ||
        MH_CreateHook(vtable[k_dispatch], reinterpret_cast<void*>(&dispatch),
                      reinterpret_cast<void**>(&g_dispatch)) != MH_OK ||
        MH_CreateHook(vtable[k_dispatch_indirect], reinterpret_cast<void*>(&dispatch_indirect),
                      reinterpret_cast<void**>(&g_dispatch_indirect)) != MH_OK ||
        MH_CreateHook(vtable[k_copy_subresource_region], reinterpret_cast<void*>(&copy_region),
                      reinterpret_cast<void**>(&g_copy_region)) != MH_OK ||
        MH_CreateHook(vtable[k_copy_resource], reinterpret_cast<void*>(&copy), reinterpret_cast<void**>(&g_copy)) !=
            MH_OK ||
        MH_CreateHook(vtable[k_update_subresource], reinterpret_cast<void*>(&update),
                      reinterpret_cast<void**>(&g_update)) != MH_OK) {
        logger::error("[context] could not hook the immediate context");
        return false;
    }
    for (const auto slot : {k_draw_indexed, k_draw, k_map, k_unmap, k_om_set_render_targets,
                            k_om_set_render_targets_and_uavs, k_cs_set_uavs, k_draw_indexed_instanced,
                            k_draw_instanced, k_begin, k_end, k_set_predication, k_draw_indexed_instanced_indirect,
                            k_draw_instanced_indirect, k_dispatch, k_dispatch_indirect, k_copy_subresource_region,
                            k_copy_resource, k_update_subresource}) {
        MH_EnableHook(vtable[slot]);
    }
    g_installed = true;
    logger::info("[context] immediate context hooked (Map, Unmap, Draw, DrawIndexed, RT/UAV binding)");
    return true;
}

void add_observer(const Observer& observer) {
    const int index = g_observer_count;
    if (index < 4) {
        g_observers[index] = observer;
        g_observer_count = index + 1;
    }
}

void set_draw_observation(bool enabled) {
    g_observe_draws = enabled;
}

void set_draw_interceptor(const DrawInterceptor& interceptor) {
    g_intercepting = false;
    g_interceptor = interceptor;
    g_intercepting = interceptor.before != nullptr;
}

void set_write_observation(bool enabled) {
    g_observe_writes = enabled;
}

HRESULT map_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                     UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    return g_map(context, resource, subresource, type, flags, mapped);
}

void update_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box,
                     const void* data, UINT row_pitch, UINT depth_pitch) {
    g_update(context, resource, subresource, box, data, row_pitch, depth_pitch);
}

uint64_t counter(Counter which) {
    return g_counters[which];
}

uint64_t read_map_ticks() {
    return t_read_map_ticks;
}

void unmap_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) {
    g_unmap(context, resource, subresource);
}
} // namespace hooks::context
