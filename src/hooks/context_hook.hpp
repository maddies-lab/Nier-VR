#pragma once

#include <cstdint>

#include <d3d11.h>

// Hooks on the game's immediate context (Map, Unmap, Draw, DrawIndexed, render
// target / UAV binding) shared by
// the modules that need them; MinHook allows one hook per function.
namespace hooks::context {
struct Observer {
    // Called after Map returns.
    void (*on_map)(ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* mapped,
                   HRESULT result) = nullptr;
    // Called before Unmap.
    void (*on_unmap)(ID3D11Resource* resource) = nullptr;
    // Called before Draw / DrawIndexed ("Draw" or "DrawIndexed").
    void (*on_draw)(const char* kind) = nullptr;
    // Called with every render target / UAV bound for writing (OMSetRenderTargets,
    // OMSetRenderTargetsAndUnorderedAccessViews, CSSetUnorderedAccessViews), while
    // write observation is enabled. Entries may be null.
    void (*on_write_views)(ID3D11View* const* views, UINT count) = nullptr;
    // Called before every draw call while draw observation is enabled, with the
    // call's arguments (unused ones 0).
    void (*on_draw_detail)(ID3D11DeviceContext* context, int kind, const UINT (&args)[5]) = nullptr;
    // Called before UpdateSubresource.
    void (*on_update)(ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box, const void* data) = nullptr;
};

// Idempotent. Hooks the context's vtable functions on first call.
bool install(ID3D11DeviceContext* context);

// Up to four observers; draw callbacks only run while draw observation is enabled.
void add_observer(const Observer& observer);
void set_draw_observation(bool enabled);
void set_write_observation(bool enabled);

// The original (unhooked) Map/Unmap, for uploads made by the mod itself.
HRESULT map_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, D3D11_MAP type,
                     UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped);
void unmap_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource);
void update_original(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource, const D3D11_BOX* box,
                     const void* data, UINT row_pitch, UINT depth_pitch);

// One module may intercept draws: `before` runs before every draw call (with its
// vertex or index count per instance) and returns true to skip it; `after` runs
// after draws that were not skipped.
struct DrawInterceptor {
    bool (*before)(ID3D11DeviceContext* context, UINT count) = nullptr;
    void (*after)(ID3D11DeviceContext* context) = nullptr;
};
void set_draw_interceptor(const DrawInterceptor& interceptor);

// Calls counted on the context since start (both-eyes pass comparison).
enum Counter {
    count_draw,         // Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced
    count_draw_indirect, // DrawInstancedIndirect, DrawIndexedInstancedIndirect
    count_dispatch,     // Dispatch, DispatchIndirect
    count_predication,  // SetPredication with a predicate
    count_query_begin,
    count_query_end,
    count_update_subresource,
    count_copy, // CopyResource, CopySubresourceRegion
    count_kinds
};
uint64_t counter(Counter which);

// Total QPC ticks the calling thread has spent inside Map(D3D11_MAP_READ*) calls
// (CPU waiting for the GPU on read-backs).
uint64_t read_map_ticks();
} // namespace hooks::context
