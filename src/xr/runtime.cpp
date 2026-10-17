#include "xr/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
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
// One swapchain per eye. Slot 0 also carries mono and theater frames.
struct EyeImage {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    bool ready = false;          // holds at least one released game frame
    transport::FrameMeta meta;   // how that frame was rendered
};
EyeImage g_eye_images[2];
XrPosef g_screen_pose{{0, 0, 0, 1}, {0, 0, -k_screen_distance}};

// Written by the XR thread, read by game threads in begin_render_view.
std::mutex g_view_mutex;
XrFovf g_eye_fov[2]{};
bool g_have_eye_fov = false;
float g_reference_yaw = 0.0f;        // head yaw that maps to the game camera's forward
math::Vec3 g_reference_position{};   // head position that maps to the game camera's eye
std::atomic<int64_t> g_predicted_time{0};
std::atomic<int64_t> g_predicted_period{0};
std::atomic<bool> g_head_tracking{false};
// F5 cycles: 0 = mono, 1 = alternate eyes, 2 = both eyes every frame.
std::atomic<int> g_stereo_mode{2};
int g_next_eye = 0;               // game thread: alternate mode's next eye
bool g_stereo_key_was_down = false; // render thread only

// The view rendered this frame (game threads), sent with the frame at Present.
// What the latest main-camera update rendered (game thread writes).
struct FrameViews {
    transport::FrameMeta primary;   // left eye (stereo) or head (mono)
    transport::FrameMeta secondary; // right eye, both-eyes mode
    bool both = false;
    bool has_secondary_scene = false;
    float secondary_scene[64]{};    // right eye SceneBuffer block
    uint64_t sequence = 0;          // bumped per camera update
};
std::mutex g_frame_meta_mutex;
FrameViews g_frame_views;

// The views of the render queue the next draw job executes (latched at Present).
std::mutex g_job_mutex;
FrameViews g_job_views;
bool g_job_enabled = false;
std::atomic<uint64_t> g_both_rendered_sequence{0};
IDXGISwapChain* g_game_swapchain = nullptr;

// Render thread: which pose goes with a presented image. The camera update runs
// on the game's update thread a few ms after each Present, while the render thread
// draws with the camera from the update before; so by default the pose latched at
// the previous Present goes with this one (F6 switches to "newest").
std::atomic<int> g_pairing{1}; // 0 = newest pose, 1 = pose latched at the previous Present
FrameViews g_latched;
uint64_t g_used_sequence = 0;
bool g_pairing_key_was_down = false;
bool g_head_tracking_key_was_down = false; // render thread only

// Render thread: hysteresis between theater and projected presentation, so screens
// that update the main camera only on some frames (pause menu map) don't flicker.
constexpr int k_projected_enter_frames = 3;
constexpr int k_projected_leave_frames = 10;
bool g_presenting_projected = false;
int g_meta_streak = 0, g_miss_streak = 0;
transport::FrameMeta g_last_meta, g_last_secondary;

// Frame-order trace (camera update vs Present), armed by F7.
std::atomic<int> g_trace_remaining{0};

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

bool create_swapchain(EyeImage& eye, uint32_t width, uint32_t height, DXGI_FORMAT source_format) {
    if (eye.swapchain != XR_NULL_HANDLE) {
        fn::xrDestroySwapchain(eye.swapchain);
        eye.swapchain = XR_NULL_HANDLE;
    }
    eye.images.clear();
    eye.ready = false;

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
    if (!check(fn::xrCreateSwapchain(g_session, &info, &eye.swapchain), "xrCreateSwapchain")) {
        return false;
    }

    fn::xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    fn::xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    for (const auto& image : images) {
        eye.images.push_back(image.texture);
    }

    eye.width = width;
    eye.height = height;
    eye.source_format = source_format;
    {
        std::scoped_lock _{g_status_mutex};
        g_status.width = width;
        g_status.height = height;
    }
    logger::info("[xr] swapchain %ux%u, format %lld, %u images", width, height, (long long)chosen, count);
    return true;
}

// Copies the newest game frame into its eye's swapchain, if there is one.
void consume_game_frame(int channel) {
    const auto frame = transport::claim_newest(channel, g_xr_device);
    if (!frame) {
        return;
    }

    auto& eye = g_eye_images[frame->meta.eye == 1 ? 1 : 0];
    if (eye.swapchain == XR_NULL_HANDLE || frame->width != eye.width || frame->height != eye.height ||
        frame->format != eye.source_format) {
        if (!create_swapchain(eye, frame->width, frame->height, frame->format)) {
            transport::release(*frame);
            return;
        }
    }

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!check(fn::xrAcquireSwapchainImage(eye.swapchain, &acquire, &index), "xrAcquireSwapchainImage")) {
        transport::release(*frame);
        return;
    }
    if (!check(fn::xrWaitSwapchainImage(eye.swapchain, &wait), "xrWaitSwapchainImage")) {
        transport::release(*frame);
    } else {
        transport::copy_and_release(*frame, g_xr_context, eye.images.at(index));
        eye.meta = frame->meta;
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (check(fn::xrReleaseSwapchainImage(eye.swapchain, &release_info), "xrReleaseSwapchainImage")) {
        eye.ready = true;
    }
}

XrSwapchainSubImage sub_image(const EyeImage& eye) {
    XrSwapchainSubImage image{};
    image.swapchain = eye.swapchain;
    image.imageRect = {{0, 0}, {static_cast<int32_t>(eye.width), static_cast<int32_t>(eye.height)}};
    return image;
}

void fill_projection_view(XrCompositionLayerProjectionView& view, const EyeImage& eye) {
    const auto& meta = eye.meta;
    view.pose.orientation = {meta.orientation[0], meta.orientation[1], meta.orientation[2], meta.orientation[3]};
    view.pose.position = {meta.position[0], meta.position[1], meta.position[2]};
    view.fov = {std::atan(meta.tan_left), std::atan(meta.tan_right), std::atan(meta.tan_up), std::atan(meta.tan_down)};
    view.subImage = sub_image(eye);
}

// Puts the screen straight ahead of the head, level, yaw only, and makes this head
// pose the reference that maps onto the game camera in head-tracked mode.
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
    {
        std::scoped_lock _{g_view_mutex};
        g_reference_yaw = yaw;
        g_reference_position = {head.x, head.y, head.z};
    }
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
        g_predicted_time = frame.predictedDisplayTime;
        g_predicted_period = frame.predictedDisplayPeriod;

        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frame.predictedDisplayTime;
        locate.space = g_local;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t view_count = 0;
        if (XR_SUCCEEDED(fn::xrLocateViews(g_session, &locate, &view_state, 2, &view_count, views)) &&
            view_count == 2) {
            std::scoped_lock _{g_view_mutex};
            g_eye_fov[0] = views[0].fov;
            g_eye_fov[1] = views[1].fov;
            g_have_eye_fov = true;
        }

        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        start = Clock::now();
        const auto begun = fn::xrBeginFrame(g_session, &begin);
        begin_time.add(ms_since(start));
        if (!check(begun, "xrBeginFrame")) {
            continue;
        }

        start = Clock::now();
        consume_game_frame(0);
        consume_game_frame(1);
        copy_time.add(ms_since(start));

        if (g_recenter && frame.shouldRender) {
            place_screen(frame.predictedDisplayTime);
        }

        // Re-submitting the last released image is valid when the game has not
        // produced a new frame since the previous headset frame.
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjectionView projection_views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1]{};
        uint32_t layer_count = 0;
        const auto& left = g_eye_images[0];
        const auto& right = g_eye_images[1];
        const char* presentation = "none";
        if (frame.shouldRender && left.ready) {
            if (left.meta.projected && left.meta.eye == 0 && right.ready && right.meta.projected &&
                right.meta.eye == 1) {
                // Stereo: each eye's own image, pose and frustum (rendered on alternate game frames).
                fill_projection_view(projection_views[0], left);
                fill_projection_view(projection_views[1], right);
                presentation = "stereo";
            } else if (left.meta.projected && left.meta.eye <= 0) {
                // Mono: both eyes get the one image, rendered from the head pose with a
                // frustum covering both eyes; the compositor reprojects it per eye.
                fill_projection_view(projection_views[0], left);
                fill_projection_view(projection_views[1], left);
                presentation = "mono";
            } else {
                quad.space = g_local;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage = sub_image(left);
                quad.pose = g_screen_pose;
                quad.size = {k_screen_width, k_screen_width * left.height / left.width};
                layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
                presentation = "theater";
            }
            if (layers[0] == nullptr) {
                projection.space = g_local;
                projection.viewCount = 2;
                projection.views = projection_views;
                layers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
            }
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
        if (layer_count > 0 && presentation != g_status.presentation) {
            std::scoped_lock _{g_status_mutex};
            g_status.presentation = presentation;
            g_status.projected = std::string_view{presentation} != "theater";
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
    FrameViews newest;
    {
        std::scoped_lock _{g_frame_meta_mutex};
        newest = g_frame_views;
    }

    // The queue drawn in this iteration was built from the update before the newest
    // (see g_pairing); the next draw job executes the one built from the newest.
    const FrameViews candidate = g_pairing == 1 ? g_latched : newest;
    g_latched = newest;

    std::optional<transport::FrameMeta> fresh;
    if (candidate.sequence != 0 && candidate.sequence != g_used_sequence) {
        fresh = candidate.primary;
        g_used_sequence = candidate.sequence;
    }

    if (fresh) {
        ++g_meta_streak;
        g_miss_streak = 0;
        g_last_meta = *fresh;
    } else {
        ++g_miss_streak;
        g_meta_streak = 0;
    }
    if (!g_presenting_projected && g_meta_streak >= k_projected_enter_frames) {
        g_presenting_projected = true;
    } else if (g_presenting_projected && g_miss_streak >= k_projected_leave_frames) {
        g_presenting_projected = false;
    }

    {
        std::scoped_lock _{g_job_mutex};
        g_job_views = newest;
        g_job_enabled = g_presenting_projected;
    }

    // Both eyes: the draw job already published the left image; the backbuffer holds
    // the right one. Otherwise one image: left/alternate eye, head (mono) or theater.
    bool ok;
    if (g_presenting_projected && candidate.both && g_both_rendered_sequence == candidate.sequence) {
        if (fresh) {
            g_last_secondary = candidate.secondary;
        }
        ok = transport::publish(1, g_game_device, g_game_context, backbuffer, g_last_secondary);
    } else {
        // A frame without its own pose reuses the last one (slightly stale, never a mode flip).
        const transport::FrameMeta meta =
            g_presenting_projected ? (fresh ? *fresh : g_last_meta) : transport::FrameMeta{};
        ok = transport::publish(0, g_game_device, g_game_context, backbuffer, meta);
    }
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
        g_game_swapchain = swapchain;
        // Instance creation can block (wineopenxr waits on Proton's VR setup), so keep it off this thread.
        std::thread{xr_thread}.detach();
        return;
    }

    const bool recenter_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (recenter_down && !g_recenter_key_was_down) {
        request_recenter();
    }
    g_recenter_key_was_down = recenter_down;

    const bool head_tracking_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (head_tracking_down && !g_head_tracking_key_was_down) {
        g_head_tracking = !g_head_tracking;
        g_recenter = true;
        g_trace_remaining = 40;
        logger::info("[xr] head tracking %s", g_head_tracking ? "on" : "off");
    }
    g_head_tracking_key_was_down = head_tracking_down;

    const bool pairing_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (pairing_down && !g_pairing_key_was_down) {
        g_pairing = 1 - g_pairing;
        logger::info("[xr] pose pairing: %s", g_pairing ? "previous Present (pipelined)" : "newest");
    }
    g_pairing_key_was_down = pairing_down;

    const bool stereo_down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (stereo_down && !g_stereo_key_was_down) {
        g_stereo_mode = (g_stereo_mode + 1) % 3;
        logger::info("[xr] render: %s", stereo_mode_name(g_stereo_mode));
    }
    g_stereo_key_was_down = stereo_down;

    trace("present");
    static int present_count = 0;
    if (++present_count == 600) {
        logger::backtrace("Present");
    }

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
        g_status.head_tracking = g_head_tracking;
        g_status.pairing = g_pairing;
        g_status.stereo_mode = stereo_mode_name(g_stereo_mode);
        g_game_frames = 0;
        g_dropped_frames = 0;
        g_game_window_start = Clock::now();
    }
}

const char* stereo_mode_name(int mode) {
    switch (mode) {
    case 0:
        return "mono";
    case 1:
        return "stereo, alternate eyes";
    default:
        return "stereo, both eyes";
    }
}

void trace(const char* event) {
    if (g_trace_remaining.load() > 0 && g_trace_remaining.fetch_sub(1) > 0) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        logger::info("[trace] %-8s thread %5lu  t=%.3f ms", event, GetCurrentThreadId(),
                     now.QuadPart * 1000.0 / frequency.QuadPart);
    }
}

void request_recenter() {
    g_recenter = true;
}

namespace {
transport::FrameMeta make_meta(const XrPosef& pose, int eye, const RenderView& view) {
    transport::FrameMeta meta;
    meta.projected = true;
    meta.eye = eye;
    meta.orientation[0] = pose.orientation.x;
    meta.orientation[1] = pose.orientation.y;
    meta.orientation[2] = pose.orientation.z;
    meta.orientation[3] = pose.orientation.w;
    meta.position[0] = pose.position.x;
    meta.position[1] = pose.position.y;
    meta.position[2] = pose.position.z;
    meta.tan_left = view.tan_left;
    meta.tan_right = view.tan_right;
    meta.tan_up = view.tan_up;
    meta.tan_down = view.tan_down;
    return meta;
}

void set_relative_pose(RenderView& view, const XrPosef& pose, float reference_yaw, const math::Vec3& reference) {
    const auto& o = pose.orientation;
    const auto& p = pose.position;
    const math::Quat to_reference = math::yaw_rotation(-reference_yaw);
    view.head_orientation = to_reference * math::Quat{o.x, o.y, o.z, o.w};
    view.head_position = math::rotate(to_reference, math::Vec3{p.x, p.y, p.z} - reference);
}

// Smallest symmetric frustum containing the given eye fovs. Symmetric because the
// game's screen-space passes (shadow mask, lighting, AO) rebuild positions from
// depth with g_CameraParam = (near, far, tan(hfov/2), tan(vfov/2)), which assumes a
// centred frustum: asymmetric per-eye frustums put shadows in the wrong place.
// Squeezed into the game's 16:9 frame (anamorphic); the compositor maps it back.
void set_symmetric_cover(RenderView& view, const XrFovf* fovs, int count) {
    float half_width = 0.0f, half_height = 0.0f;
    for (int i = 0; i < count; ++i) {
        half_width = std::max({half_width, -std::tan(fovs[i].angleLeft), std::tan(fovs[i].angleRight)});
        half_height = std::max({half_height, std::tan(fovs[i].angleUp), -std::tan(fovs[i].angleDown)});
    }
    view.tan_left = -half_width;
    view.tan_right = half_width;
    view.tan_up = half_height;
    view.tan_down = -half_height;
}
} // namespace

int begin_render_views(float aspect, RenderView& primary, RenderView& secondary) {
    const XrTime predicted = g_predicted_time;
    if (!g_head_tracking || !g_running || predicted == 0) {
        return 0;
    }

    // The frame rendered now reaches the compositor about one headset frame later.
    const XrTime time = predicted + g_predicted_period;

    XrFovf fov[2];
    float reference_yaw;
    math::Vec3 reference_position;
    {
        std::scoped_lock _{g_view_mutex};
        if (!g_have_eye_fov) {
            return 0;
        }
        fov[0] = g_eye_fov[0];
        fov[1] = g_eye_fov[1];
        reference_yaw = g_reference_yaw;
        reference_position = g_reference_position;
    }

    const int mode = g_stereo_mode;
    FrameViews views;
    int count = 1;
    if (mode == 0) {
        // Mono: the head pose, one frustum covering both eyes widened to the game's aspect.
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(fn::xrLocateSpace(g_view, g_local, time, &location)) ||
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
            return 0;
        }
        set_relative_pose(primary, location.pose, reference_yaw, reference_position);
        float left = std::min(std::tan(fov[0].angleLeft), std::tan(fov[1].angleLeft));
        float right = std::max(std::tan(fov[0].angleRight), std::tan(fov[1].angleRight));
        float up = std::max(std::tan(fov[0].angleUp), std::tan(fov[1].angleUp));
        float down = std::min(std::tan(fov[0].angleDown), std::tan(fov[1].angleDown));
        const float width = right - left, height = up - down;
        if (width / height < aspect) {
            const float extra = (aspect * height - width) * 0.5f;
            left -= extra;
            right += extra;
        } else {
            const float extra = (width / aspect - height) * 0.5f;
            up += extra;
            down -= extra;
        }
        primary.tan_left = left;
        primary.tan_right = right;
        primary.tan_up = up;
        primary.tan_down = down;
        views.primary = make_meta(location.pose, -1, primary);
    } else {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = time;
        locate.space = g_local;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t located = 0;
        if (XR_FAILED(fn::xrLocateViews(g_session, &locate, &state, 2, &located, eyes)) || located != 2 ||
            (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            return 0;
        }
        const XrFovf eye_fovs[2] = {eyes[0].fov, eyes[1].fov};
        if (mode == 1) {
            // Alternate eyes: one eye per update, its own symmetric cover.
            const int eye = g_next_eye;
            g_next_eye ^= 1;
            set_relative_pose(primary, eyes[eye].pose, reference_yaw, reference_position);
            set_symmetric_cover(primary, &eye_fovs[eye], 1);
            views.primary = make_meta(eyes[eye].pose, eye, primary);
        } else {
            // Both eyes: left rendered by the game, right by re-running the draw job.
            // One cover for both, so the projection (and g_CameraParam) is shared.
            set_relative_pose(primary, eyes[0].pose, reference_yaw, reference_position);
            set_relative_pose(secondary, eyes[1].pose, reference_yaw, reference_position);
            set_symmetric_cover(primary, eye_fovs, 2);
            set_symmetric_cover(secondary, eye_fovs, 2);
            views.primary = make_meta(eyes[0].pose, 0, primary);
            views.secondary = make_meta(eyes[1].pose, 1, secondary);
            views.both = true;
            count = 2;
        }
    }

    std::scoped_lock _{g_frame_meta_mutex};
    views.sequence = g_frame_views.sequence + 1;
    g_frame_views = views;
    return count;
}

void set_secondary_scene(const float (&scene)[64]) {
    std::scoped_lock _{g_frame_meta_mutex};
    std::copy(std::begin(scene), std::end(scene), g_frame_views.secondary_scene);
    g_frame_views.has_secondary_scene = true;
}

bool job_views(JobViews& out) {
    std::scoped_lock _{g_job_mutex};
    if (!g_job_enabled || !g_job_views.both || !g_job_views.has_secondary_scene) {
        return false;
    }
    out.sequence = g_job_views.sequence;
    out.primary = g_job_views.primary;
    std::copy(std::begin(g_job_views.secondary_scene), std::end(g_job_views.secondary_scene), out.secondary_scene);
    return true;
}

bool publish_from_job(const transport::FrameMeta& meta) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (g_game_swapchain == nullptr ||
        FAILED(g_game_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)))) {
        return false;
    }
    const bool ok = transport::publish(0, g_game_device, g_game_context, backbuffer, meta);
    backbuffer->Release();
    return ok;
}

void mark_both_rendered(uint64_t sequence) {
    g_both_rendered_sequence = sequence;
}

ID3D11DeviceContext* game_context() {
    return g_game_context;
}

Status status() {
    std::scoped_lock _{g_status_mutex};
    return g_status;
}
} // namespace xr
