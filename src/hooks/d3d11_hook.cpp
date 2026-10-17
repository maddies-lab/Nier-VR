#include "hooks/d3d11_hook.hpp"

#include <d3d11.h>
#include <MinHook.h>

#include "log.hpp"

namespace hooks {
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr size_t k_present_index = 8;
constexpr size_t k_resize_buffers_index = 13;

D3D11Callbacks g_callbacks;
PresentFn g_present_original = nullptr;
ResizeBuffersFn g_resize_buffers_original = nullptr;

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swapchain, UINT sync_interval, UINT flags) {
    if (g_callbacks.on_present) {
        g_callbacks.on_present(swapchain);
    }
    return g_present_original(swapchain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE resize_buffers_hook(IDXGISwapChain* swapchain, UINT count, UINT width, UINT height,
                                              DXGI_FORMAT format, UINT flags) {
    if (g_callbacks.on_pre_resize) {
        g_callbacks.on_pre_resize(swapchain);
    }
    return g_resize_buffers_original(swapchain, count, width, height, format, flags);
}

// Swapchain vtables are shared per implementation, so a throwaway swapchain on a
// hidden window yields the same Present the game calls (DXVK under Proton too).
bool read_swapchain_vtable(void** out_present, void** out_resize) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NieRVRDummyWindow";
    RegisterClassExW(&wc);
    const auto hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr,
                                      nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const auto hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                  D3D11_SDK_VERSION, &desc, &swapchain, &device, nullptr, &context);

    bool ok = false;
    if (SUCCEEDED(hr)) {
        const auto vtable = *reinterpret_cast<void***>(swapchain);
        *out_present = vtable[k_present_index];
        *out_resize = vtable[k_resize_buffers_index];
        ok = true;
        swapchain->Release();
        context->Release();
        device->Release();
    } else {
        logger::error("[d3d11] dummy D3D11CreateDeviceAndSwapChain failed: 0x%08lx", (unsigned long)hr);
    }

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
} // namespace

bool install_d3d11(D3D11Callbacks callbacks) {
    g_callbacks = std::move(callbacks);

    void* present = nullptr;
    void* resize = nullptr;
    if (!read_swapchain_vtable(&present, &resize)) {
        return false;
    }

    if (MH_CreateHook(present, reinterpret_cast<void*>(&present_hook),
                      reinterpret_cast<void**>(&g_present_original)) != MH_OK ||
        MH_CreateHook(resize, reinterpret_cast<void*>(&resize_buffers_hook),
                      reinterpret_cast<void**>(&g_resize_buffers_original)) != MH_OK) {
        logger::error("[d3d11] MH_CreateHook failed");
        return false;
    }

    if (MH_EnableHook(present) != MH_OK || MH_EnableHook(resize) != MH_OK) {
        logger::error("[d3d11] MH_EnableHook failed");
        return false;
    }

    logger::info("[d3d11] Present %p and ResizeBuffers %p hooked", present, resize);
    return true;
}
} // namespace hooks
