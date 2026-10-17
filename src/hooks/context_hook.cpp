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

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);

DrawIndexedFn g_draw_indexed = nullptr;
DrawFn g_draw = nullptr;
MapFn g_map = nullptr;
UnmapFn g_unmap = nullptr;
std::atomic<bool> g_installed{false};

Observer g_observers[4];
std::atomic<int> g_observer_count{0};
std::atomic<bool> g_observe_draws{false};

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
    const auto hr = g_map(self, resource, subresource, type, flags, mapped);
    for (int i = 0; i < g_observer_count; ++i) {
        if (g_observers[i].on_map) {
            g_observers[i].on_map(resource, type, mapped, hr);
        }
    }
    return hr;
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
        MH_CreateHook(vtable[k_unmap], reinterpret_cast<void*>(&unmap), reinterpret_cast<void**>(&g_unmap)) != MH_OK) {
        logger::error("[context] could not hook the immediate context");
        return false;
    }
    for (const auto slot : {k_draw_indexed, k_draw, k_map, k_unmap}) {
        MH_EnableHook(vtable[slot]);
    }
    g_installed = true;
    logger::info("[context] immediate context hooked (Map, Unmap, Draw, DrawIndexed)");
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

HRESULT map_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                     UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped) {
    return (g_map ? g_map : nullptr)(context, resource, subresource, type, flags, mapped);
}

void unmap_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) {
    g_unmap(context, resource, subresource);
}
} // namespace hooks::context
