cmake_minimum_required(VERSION 3.24)
project(KabelCam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Laufzeitbibliothek fest einbinden: Auf dem PC des Nutzers muss nichts zusätzlich installiert werden.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/utf-8 /permissive- /EHsc /W4 /MP)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX KABELCAM_VERSION="${PROJECT_VERSION}")

# Dear ImGui für die Oberfläche (klein, schnell, ohne Laufzeit-Abhängigkeiten).
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
set_target_properties(imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

# Gemeinsamer Kern: USB-Tunnel, Protokoll, Decoder.
add_library(kabelcam_core STATIC
    src/FrameStore.cpp
    src/H264Decoder.cpp
    src/Json.cpp
    src/Log.cpp
    src/Net.cpp
    src/Plist.cpp
    src/SetupCheck.cpp
    src/Snapshot.cpp
    src/StreamClient.cpp
    src/Usbmux.cpp
    src/VideoView.cpp)
target_include_directories(kabelcam_core PUBLIC src)
target_link_libraries(kabelcam_core PUBLIC
    ws2_32 d3d11 dxgi mfplat mfuuid wmcodecdspuuid windowscodecs ole32 shell32 advapi32)

add_executable(kabelcam WIN32 src/main.cpp src/App.cpp)
target_link_libraries(kabelcam PRIVATE kabelcam_core imgui d3dcompiler dwmapi)

add_executable(kabelcam_fakephone tools/fakephone.cpp)
target_link_libraries(kabelcam_fakephone PRIVATE kabelcam_core winmm)
