cmake_minimum_required(VERSION 3.24)
project(KrakznCam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Laufzeitbibliothek fest einbinden: Auf dem PC des Nutzers muss nichts zusätzlich installiert werden.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_options(/utf-8 /permissive- /EHsc /W4 /MP)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DEFINE_MATH_OPERATORS
    KRAKZNCAM_VERSION="${PROJECT_VERSION}")

# Dear ImGui für die Oberfläche (klein, schnell, ohne Laufzeit-Abhängigkeiten).
# 1.92 rendert Schrift in jeder Größe scharf – wichtig für die großen, dünnen Titel.
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b
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
add_library(krakzncam_core STATIC
    src/AutoRenew.cpp
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
    src/VideoView.cpp
    src/VirtualCamera.cpp)
target_include_directories(krakzncam_core PUBLIC src)
target_link_libraries(krakzncam_core PUBLIC
    ws2_32 d3d11 dxgi mfplat mfuuid wmcodecdspuuid windowscodecs ole32 shell32 advapi32 uuid)

add_executable(krakzncam WIN32 src/main.cpp src/App.cpp src/Theme.cpp res/krakzncam.rc)
target_link_libraries(krakzncam PRIVATE krakzncam_core imgui d3dcompiler dwmapi)

# Virtuelle Kamera (Media Source für den Frame-Server-Dienst von Windows 11).
add_library(krakzncam_vcam SHARED vcam/VirtualCamera.cpp vcam/Placeholder.cpp vcam/krakzncam_vcam.def)
target_include_directories(krakzncam_vcam PRIVATE src)
target_link_libraries(krakzncam_vcam PRIVATE mfplat mfuuid advapi32 gdi32 ole32)

# Prüfwerkzeug für die virtuelle Kamera (liest Bilder wie OBS und speichert eins als PNG).
add_executable(krakzncam_vcamtest tools/vcamtest.cpp)
target_include_directories(krakzncam_vcamtest PRIVATE src)
target_link_libraries(krakzncam_vcamtest PRIVATE mfplat mfuuid mfreadwrite mf windowscodecs ole32)

add_executable(krakzncam_iconmaker tools/IconMaker.cpp)
target_link_libraries(krakzncam_iconmaker PRIVATE d2d1 windowscodecs ole32)

add_executable(krakzncam_fakephone tools/fakephone.cpp tools/TestScene.cpp)
target_link_libraries(krakzncam_fakephone PRIVATE krakzncam_core winmm d2d1 dwrite)
