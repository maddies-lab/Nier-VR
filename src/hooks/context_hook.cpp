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

Observer g_observers[4];
std::atomic<int> g_observer_count{0};
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

void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* self, UINT count, UINT start, INT base) {
    notify_draw("DrawIndexed");
    g_draw_indexed(self, count, start, base);
}

void STDMETHODCALLTYPE draw(ID3D11DeviceContext* self, UINT count, UINT start) {
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
                      reinterpret_cast<void**>(&g_cs_set_uav)) != MH_OK) {
        logger::error("[context] could not hook the immediate context");
        return false;
    }
    for (const auto slot : {k_draw_indexed, k_draw, k_map, k_unmap, k_om_set_render_targets,
                            k_om_set_render_targets_and_uavs, k_cs_set_uavs}) {
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

void set_write_observation(bool enabled) {
    g_observe_writes = enabled;
}

HRESULT map_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                     UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    return g_map(context, resource, subresource, type, flags, mapped);
}

uint64_t read_map_ticks() {
    return t_read_map_ticks;
}

void unmap_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) {
    g_unmap(context, resource, subresource);
}
} // namespace hooks::context
