cmake_minimum_required(VERSION 3.25)
project(nier_vr LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Name of the system DLL this build impersonates. Generated stubs live in
# src/proxy/generated (see tools/gen_proxy.py).
set(NIER_VR_PROXY "winmm" CACHE STRING "Proxy DLL name")

include(FetchContent)

FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
    SOURCE_SUBDIR _unused)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz)
# Khronos Windows loader: headers to build against, openxr_loader.dll to ship.
FetchContent_Declare(openxr_loader
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.62/openxr_loader_windows-1.1.62.zip
    URL_HASH SHA256=800ec772e2f9448a26ab9f579f4914d984346dd9d0d7c007841abe21d2c8ff2f
    SOURCE_SUBDIR _unused)
FetchContent_MakeAvailable(minhook imgui openxr_loader)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi)

set(PROXY_DIR ${CMAKE_SOURCE_DIR}/src/proxy/generated)

add_library(nier_vr SHARED
    src/dllmain.cpp
    src/log.cpp
    src/scan.cpp
    src/overlay.cpp
    src/game/build.cpp
    src/game/camera.cpp
    src/game/camera_hook.cpp
    src/game/census.cpp
    src/game/stereo_render.cpp
    src/game/dump.cpp
    src/game/hud.cpp
    src/game/inspect.cpp
    src/game/player.cpp
    src/game/watch.cpp
    src/hooks/context_hook.cpp
    src/hooks/d3d11_hook.cpp
    src/proxy/proxy.cpp
    src/xr/frame_transport.cpp
    src/xr/runtime.cpp
    ${PROXY_DIR}/${NIER_VR_PROXY}_table.cpp
    ${PROXY_DIR}/${NIER_VR_PROXY}_stubs.S
    ${PROXY_DIR}/${NIER_VR_PROXY}.def)
target_include_directories(nier_vr PRIVATE src ${openxr_loader_SOURCE_DIR}/include)
target_link_libraries(nier_vr PRIVATE minhook imgui d3d11 dxgi)
target_compile_definitions(nier_vr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# OpenXR structs are initialised as {XR_TYPE_...}; loader entry points come from GetProcAddress.
target_compile_options(nier_vr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
# Self-contained DLL: no libgcc/libstdc++/winpthread DLLs to ship.
target_link_options(nier_vr PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
set_target_properties(nier_vr PROPERTIES PREFIX "" OUTPUT_NAME ${NIER_VR_PROXY})

add_custom_command(TARGET nier_vr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${openxr_loader_SOURCE_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:nier_vr>/openxr_loader.dll)
