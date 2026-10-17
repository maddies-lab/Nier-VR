#pragma once

#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Every OpenXR entry point the mod uses, resolved through xrGetInstanceProcAddr.
#define NIER_VR_XR_FUNCTIONS(X)            \
    X(xrDestroyInstance)                   \
    X(xrGetInstanceProperties)             \
    X(xrGetSystem)                         \
    X(xrGetSystemProperties)               \
    X(xrGetD3D11GraphicsRequirementsKHR)   \
    X(xrCreateSession)                     \
    X(xrDestroySession)                    \
    X(xrBeginSession)                      \
    X(xrEndSession)                        \
    X(xrPollEvent)                         \
    X(xrCreateReferenceSpace)              \
    X(xrLocateSpace)                       \
    X(xrEnumerateSwapchainFormats)         \
    X(xrCreateSwapchain)                   \
    X(xrDestroySwapchain)                  \
    X(xrEnumerateSwapchainImages)          \
    X(xrAcquireSwapchainImage)             \
    X(xrWaitSwapchainImage)                \
    X(xrReleaseSwapchainImage)             \
    X(xrWaitFrame)                         \
    X(xrBeginFrame)                        \
    X(xrEndFrame)                          \
    X(xrResultToString)

namespace xr::fn {
#define NIER_VR_DECLARE(name) inline PFN_##name name = nullptr;
NIER_VR_XR_FUNCTIONS(NIER_VR_DECLARE)
#undef NIER_VR_DECLARE
} // namespace xr::fn
