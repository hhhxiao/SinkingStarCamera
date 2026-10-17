cmake_minimum_required(VERSION 3.20)
project(SinkingStarCamera CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/third
    ${PROJECT_SOURCE_DIR}/third/imgui
    ${PROJECT_SOURCE_DIR}/third/imgui/backends
)

add_compile_options(/wd4819 /MP /utf-8)

# The mod dll. Deliberately not named after a system dll: see readme for why a
# winmm proxy is a trap in this title.
add_library(orthocam SHARED
    dllmain.cpp
    core/camera.cpp
    core/config.cpp
    core/d3d12_overlay.cpp
    core/overlay_grid.cpp
    core/overlay_panel.cpp
    core/scanner.cpp
    tools/logger.cpp
    tools/misc.cpp
    third/imgui/imgui.cpp
    third/imgui/imgui_draw.cpp
    third/imgui/imgui_tables.cpp
    third/imgui/imgui_widgets.cpp
    third/imgui/backends/imgui_impl_win32.cpp
    third/imgui/backends/imgui_impl_dx11.cpp
    third/imgui/backends/imgui_impl_dx12.cpp
)

target_link_libraries(orthocam psapi d3d11 d3d12 d3dcompiler dxgi)
set_target_properties(orthocam PROPERTIES OUTPUT_NAME "sinkingstar_camera" PREFIX "")

# Loader: waits for the game and injects the dll via CreateRemoteThread.
add_executable(SinkingStarCameraInjector injector/injector.cpp)
set_target_properties(SinkingStarCameraInjector PROPERTIES WIN32_EXECUTABLE FALSE)
