#include "xr/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "log.hpp"
#include "xr/frame_transport.hpp"
#include "xr/openxr.hpp"

// Two D3D11 devices, two threads:
//   render thread (game's Present, game device) - hands each backbuffer to the
//       XR device through xr::transport (shared textures + GPU-side fences).
//   XR thread (own device) - OpenXR instance/session, events and the headset
//       frame loop; copies the newest game frame into the swapchain.
// The session uses its own device because wineopenxr holds the session device's
// DXVK submission queue lock for the whole native xrEndFrame (about one display
// period on WiVRn), which starved the game's own GPU submissions when it was
// the game's device.
namespace xr {
namespace {
constexpr float k_screen_distance = 2.5f; // metres in front of the head
constexpr float k_screen_width = 3.2f;    // metres; height follows the game's aspect

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Per-thread accumulator for one timed call, published once per second.
struct Accumulator {
    double sum = 0, peak = 0;
    uint32_t count = 0;

    void add(double ms) {
        sum += ms;
        peak = std::max(peak, ms);
        ++count;
    }

    Timing take() {
        const Timing t{count > 0 ? static_cast<float>(sum / count) : 0.0f, static_cast<float>(peak)};
        *this = {};
        return t;
    }
};

template <typename T>
void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

PFN_xrGetInstanceProcAddr g_get_proc = nullptr;

std::mutex g_status_mutex;
Status g_status;

std::atomic<bool> g_started{false};
std::atomic<bool> g_running{false};
std::atomic<bool> g_recenter{true};

// Render thread only.
ID3D11Device* g_game_device = nullptr;
ID3D11DeviceContext* g_game_context = nullptr;
bool g_recenter_key_was_down = false;
Accumulator g_game_copy_time;
uint32_t g_game_frames = 0, g_dropped_frames = 0;
Clock::time_point g_game_window_start = Clock::now();

// XR thread only.
ID3D11Device* g_xr_device = nullptr;
ID3D11DeviceContext* g_xr_context = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrSpace g_view = XR_NULL_HANDLE;
XrSwapchain g_swapchain = XR_NULL_HANDLE;
std::vector<ID3D11Texture2D*> g_images;
uint32_t g_swapchain_width = 0, g_swapchain_height = 0;
DXGI_FORMAT g_swapchain_source_format = DXGI_FORMAT_UNKNOWN;
bool g_image_ready = false; // the swapchain holds at least one released game frame
XrPosef g_screen_pose{{0, 0, 0, 1}, {0, 0, -k_screen_distance}};

std::string result_string(XrResult result) {
    char buffer[XR_MAX_RESULT_STRING_SIZE]{};
    if (fn::xrResultToString != nullptr && g_instance != XR_NULL_HANDLE) {
        fn::xrResultToString(g_instance, result, buffer);
        return buffer;
    }
    return std::to_string(static_cast<int>(result));
}

void set_error(const std::string& message) {
    logger::error("[xr] %s", message.c_str());
    std::scoped_lock _{g_status_mutex};
    g_status.last_error = message;
}

bool check(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    set_error(std::string{what} + " failed: " + result_string(result));
    return false;
}

void set_state(const char* state) {
    logger::info("[xr] state: %s", state);
    std::scoped_lock _{g_status_mutex};
    g_status.state = state;
}

std::wstring module_directory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir{path};
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

// ---------------------------------------------------------------------------
// XR thread: setup

bool create_xr_device() {
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(g_game_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&adapter))) {
        release(dxgi_device);
        set_error("could not get the game's DXGI adapter");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const auto hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                      &g_xr_device, nullptr, &g_xr_context);
    release(adapter);
    release(dxgi_device);
    if (FAILED(hr)) {
        set_error("D3D11CreateDevice for the XR device failed: " + std::to_string(hr));
        return false;
    }
    return true;
}

bool load_loader() {
    const auto path = module_directory() + L"openxr_loader.dll";
    const auto loader = LoadLibraryW(path.c_str());
    if (loader == nullptr) {
        set_error("could not load openxr_loader.dll next to the mod (error " + std::to_string(GetLastError()) + ")");
        return false;
    }

    g_get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
    return g_get_proc != nullptr;
}

template <typename T>
T get_proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    g_get_proc(instance, name, &function);
    return reinterpret_cast<T>(function);
}

bool create_instance() {
    const auto enumerate_extensions = get_proc<PFN_xrEnumerateInstanceExtensionProperties>(
        XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties");
    const auto create = get_proc<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance");
    if (enumerate_extensions == nullptr || create == nullptr) {
        set_error("openxr_loader.dll exports are missing");
        return false;
    }

    uint32_t count = 0;
    if (!check(enumerate_extensions(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    enumerate_extensions(nullptr, count, &count, extensions.data());

    bool has_d3d11 = false;
    for (const auto& e : extensions) {
        has_d3d11 |= std::string_view{e.extensionName} == XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    }
    if (!has_d3d11) {
        set_error(std::string{"runtime does not offer "} + XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "NieR-VR");
    std::snprintf(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "NieR-VR");
    info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
    if (!check(create(&info, &g_instance), "xrCreateInstance")) {
        return false;
    }

#define NIER_VR_LOAD(name) fn::name = get_proc<PFN_##name>(g_instance, #name);
    NIER_VR_XR_FUNCTIONS(NIER_VR_LOAD)
#undef NIER_VR_LOAD

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    fn::xrGetInstanceProperties(g_instance, &props);
    const auto runtime = std::string{props.runtimeName} + " " +
                         std::to_string(XR_VERSION_MAJOR(props.runtimeVersion)) + "." +
                         std::to_string(XR_VERSION_MINOR(props.runtimeVersion)) + "." +
                         std::to_string(XR_VERSION_PATCH(props.runtimeVersion));
    logger::info("[xr] runtime: %s", runtime.c_str());
    std::scoped_lock _{g_status_mutex};
    g_status.runtime = runtime;
    return true;
}

// Retries until a headset is connected (the runtime reports the form factor unavailable before that).
bool get_system() {
    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    bool logged = false;
    while (true) {
        const auto result = fn::xrGetSystem(g_instance, &info, &g_system);
        if (XR_SUCCEEDED(result)) {
            break;
        }
        if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            return check(result, "xrGetSystem");
        }
        if (!logged) {
            set_state("waiting for headset");
            logged = true;
        }
        Sleep(2000);
    }

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    fn::xrGetSystemProperties(g_instance, g_system, &props);
    logger::info("[xr] system: %s", props.systemName);
    std::scoped_lock _{g_status_mutex};
    g_status.system = props.systemName;
    return true;
}

bool create_session() {
    // Required before xrCreateSession.
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!check(fn::xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &requirements),
               "xrGetD3D11GraphicsRequirementsKHR")) {
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g_xr_device;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g_system;
    if (!check(fn::xrCreateSession(g_instance, &info, &g_session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace.orientation.w = 1.0f;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!check(fn::xrCreateReferenceSpace(g_session, &space, &g_local), "xrCreateReferenceSpace(LOCAL)")) {
        return false;
    }
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return check(fn::xrCreateReferenceSpace(g_session, &space, &g_view), "xrCreateReferenceSpace(VIEW)");
}

void handle_session_state(XrSessionState state) {
    logger::info("[xr] session state %d", static_cast<int>(state));
    switch (state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
        info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (check(fn::xrBeginSession(g_session, &info), "xrBeginSession")) {
            g_running = true;
            g_recenter = true;
            set_state("running");
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        g_running = false;
        fn::xrEndSession(g_session);
        set_state("stopped");
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        g_running = false;
        set_state("lost (restart the game to reconnect)");
        break;
    default:
        break;
    }
}

void poll_events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (fn::xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            handle_session_state(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            handle_session_state(XR_SESSION_STATE_LOSS_PENDING);
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------------------
// XR thread: frames

DXGI_FORMAT srgb_variant(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default:
        return format;
    }
}

bool create_swapchain(uint32_t width, uint32_t height, DXGI_FORMAT source_format) {
    if (g_swapchain != XR_NULL_HANDLE) {
        fn::xrDestroySwapchain(g_swapchain);
        g_swapchain = XR_NULL_HANDLE;
    }
    g_images.clear();
    g_image_ready = false;

    uint32_t count = 0;
    fn::xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    fn::xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());

    // The game writes sRGB-encoded values into a UNORM backbuffer; an _SRGB
    // swapchain of the same family makes the compositor decode them correctly.
    int64_t chosen = 0;
    for (const auto wanted : {srgb_variant(source_format), source_format}) {
        for (const auto f : formats) {
            if (chosen == 0 && f == wanted) {
                chosen = f;
            }
        }
    }
    if (chosen == 0) {
        set_error("no swapchain format matches backbuffer format " + std::to_string(source_format));
        return false;
    }

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    if (!check(fn::xrCreateSwapchain(g_session, &info, &g_swapchain), "xrCreateSwapchain")) {
        return false;
    }

    fn::xrEnumerateSwapchainImages(g_swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    fn::xrEnumerateSwapchainImages(g_swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    for (const auto& image : images) {
        g_images.push_back(image.texture);
    }

    g_swapchain_width = width;
    g_swapchain_height = height;
    g_swapchain_source_format = source_format;
    {
        std::scoped_lock _{g_status_mutex};
        g_status.width = width;
        g_status.height = height;
    }
    logger::info("[xr] swapchain %ux%u, format %lld, %u images", width, height, (long long)chosen, count);
    return true;
}

// Copies the newest game frame into the swapchain, if there is one.
void consume_game_frame() {
    const auto frame = transport::claim_newest(g_xr_device);
    if (!frame) {
        return;
    }

    if (g_swapchain == XR_NULL_HANDLE || frame->width != g_swapchain_width || frame->height != g_swapchain_height ||
        frame->format != g_swapchain_source_format) {
        if (!create_swapchain(frame->width, frame->height, frame->format)) {
            transport::release(*frame);
            return;
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!check(fn::xrAcquireSwapchainImage(g_swapchain, &acquire, &index), "xrAcquireSwapchainImage")) {
        transport::release(*frame);
        return;
    }
    if (!check(fn::xrWaitSwapchainImage(g_swapchain, &wait), "xrWaitSwapchainImage")) {
        transport::release(*frame);
    } else {
        transport::copy_and_release(*frame, g_xr_context, g_images.at(index));
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (check(fn::xrReleaseSwapchainImage(g_swapchain, &release_info), "xrReleaseSwapchainImage")) {
        g_image_ready = true;
    }
}

// Puts the screen straight ahead of the head, level, yaw only.
void place_screen(XrTime time) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(fn::xrLocateSpace(g_view, g_local, time, &location)) ||
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0 ||
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0) {
        return;
    }

    // Forward = orientation * (0, 0, -1), flattened onto the floor plane.
    const auto& q = location.pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz); // rotation about +Y taking (0,0,-1) to forward

    const auto& head = location.pose.position;
    g_screen_pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    g_screen_pose.position = {head.x - std::sin(yaw) * k_screen_distance, head.y,
                              head.z - std::cos(yaw) * k_screen_distance};
    g_recenter = false;
    logger::info("[xr] screen placed at %.2f %.2f %.2f, yaw %.1f deg", g_screen_pose.position.x,
                 g_screen_pose.position.y, g_screen_pose.position.z, yaw * 57.29578f);
}

void xr_thread() {
    set_state("starting");
    if (!create_xr_device() || !load_loader() || !create_instance() || !get_system() || !create_session()) {
        set_state("failed (see NieR-VR.log)");
        return;
    }
    set_state("waiting for session");

    Accumulator wait_time, begin_time, copy_time, end_time;
    uint32_t frames = 0;
    int windows = 0;
    auto window_start = Clock::now();

    while (true) {
        poll_events();

        if (const auto elapsed = ms_since(window_start); elapsed >= 1000.0) {
            std::scoped_lock _{g_status_mutex};
            auto& s = g_status;
            s.headset_fps = static_cast<float>(frames * 1000.0 / elapsed);
            s.xr_wait = wait_time.take();
            s.xr_begin = begin_time.take();
            s.xr_copy = copy_time.take();
            s.xr_end = end_time.take();
            if (++windows % 5 == 0) {
                logger::info("[xr] perf: headset %.1f fps (wait %.2f/%.2f, begin %.2f/%.2f, copy %.2f/%.2f, "
                             "end %.2f/%.2f ms) | game %.1f fps, dropped %.1f, copy %.2f/%.2f ms",
                             s.headset_fps, s.xr_wait.avg_ms, s.xr_wait.max_ms, s.xr_begin.avg_ms, s.xr_begin.max_ms,
                             s.xr_copy.avg_ms, s.xr_copy.max_ms, s.xr_end.avg_ms, s.xr_end.max_ms, s.game_fps,
                             s.dropped_fps, s.game_copy.avg_ms, s.game_copy.max_ms);
            }
            frames = 0;
            window_start = Clock::now();
        }

        if (!g_running) {
            Sleep(20);
            continue;
        }

        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        auto start = Clock::now();
        const auto waited = fn::xrWaitFrame(g_session, &wait, &frame);
        wait_time.add(ms_since(start));
        if (!check(waited, "xrWaitFrame")) {
            Sleep(100);
            continue;
        }

        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        start = Clock::now();
        const auto begun = fn::xrBeginFrame(g_session, &begin);
        begin_time.add(ms_since(start));
        if (!check(begun, "xrBeginFrame")) {
            continue;
        }

        start = Clock::now();
        consume_game_frame();
        copy_time.add(ms_since(start));

        // Re-submitting the last released image is valid when the game has not
        // produced a new frame since the previous headset frame.
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad)};
        uint32_t layer_count = 0;
        if (frame.shouldRender && g_image_ready) {
            if (g_recenter) {
                place_screen(frame.predictedDisplayTime);
            }
            quad.space = g_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_swapchain;
            quad.subImage.imageRect = {
                {0, 0}, {static_cast<int32_t>(g_swapchain_width), static_cast<int32_t>(g_swapchain_height)}};
            quad.pose = g_screen_pose;
            quad.size = {k_screen_width, k_screen_width * g_swapchain_height / g_swapchain_width};
            layer_count = 1;
        }

        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = layer_count;
        end.layers = layers;
        start = Clock::now();
        const auto ended = fn::xrEndFrame(g_session, &end);
        end_time.add(ms_since(start));
        if (check(ended, "xrEndFrame")) {
            ++frames;
        }
    }
}

// ---------------------------------------------------------------------------
// Render thread

// Returns true if the frame was handed to the XR device.
bool hand_over_frame(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) {
        return false;
    }
    const bool ok = transport::publish(g_game_device, g_game_context, backbuffer);
    backbuffer->Release();
    return ok;
}
} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_started.exchange(true)) {
        if (FAILED(swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_game_device)))) {
            logger::error("[xr] swapchain has no D3D11 device");
            return;
        }
        g_game_device->GetImmediateContext(&g_game_context);
        // Instance creation can block (wineopenxr waits on Proton's VR setup), so keep it off this thread.
        std::thread{xr_thread}.detach();
        return;
    }

    const bool recenter_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (recenter_down && !g_recenter_key_was_down) {
        request_recenter();
    }
    g_recenter_key_was_down = recenter_down;

    if (g_running) {
        const auto start = Clock::now();
        if (hand_over_frame(swapchain)) {
            g_game_copy_time.add(ms_since(start));
            ++g_game_frames;
        } else {
            ++g_dropped_frames;
        }
    }

    if (const auto elapsed = ms_since(g_game_window_start); elapsed >= 1000.0) {
        std::scoped_lock _{g_status_mutex};
        g_status.game_fps = static_cast<float>(g_game_frames * 1000.0 / elapsed);
        g_status.dropped_fps = static_cast<float>(g_dropped_frames * 1000.0 / elapsed);
        g_status.game_copy = g_game_copy_time.take();
        g_status.transport = transport::description();
        g_game_frames = 0;
        g_dropped_frames = 0;
        g_game_window_start = Clock::now();
    }
}

void request_recenter() {
    g_recenter = true;
}

Status status() {
    std::scoped_lock _{g_status_mutex};
    return g_status;
}
} // namespace xr
