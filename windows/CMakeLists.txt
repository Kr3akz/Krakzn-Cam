cmake_minimum_required(VERSION 3.24)
# Version: beim Release aus dem Tag (z. B. v0.2.0 -> -DKRAKZNCAM_RELEASE_VERSION=0.2.0), sonst Entwicklungsstand.
set(KRAKZNCAM_RELEASE_VERSION "" CACHE STRING "Version aus dem Release-Tag, z. B. 0.2.0")
if(NOT KRAKZNCAM_RELEASE_VERSION STREQUAL "")
    if(NOT KRAKZNCAM_RELEASE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "KRAKZNCAM_RELEASE_VERSION muss wie 0.2.0 aussehen, ist aber '${KRAKZNCAM_RELEASE_VERSION}'")
    endif()
    set(KRAKZNCAM_PROJECT_VERSION ${KRAKZNCAM_RELEASE_VERSION})
else()
    set(KRAKZNCAM_PROJECT_VERSION 0.1.0)
endif()
project(KrakznCam VERSION ${KRAKZNCAM_PROJECT_VERSION} LANGUAGES CXX)

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
    src/Http.cpp
    src/H264Decoder.cpp
    src/Json.cpp
    src/Log.cpp
    src/Net.cpp
    src/Plist.cpp
    src/SetupCheck.cpp
    src/Snapshot.cpp
    src/StreamClient.cpp
    src/Updater.cpp
    src/Usbmux.cpp
    src/VideoView.cpp
    src/VirtualCamera.cpp
    src/CameraFeeder.cpp)
target_include_directories(krakzncam_core PUBLIC src)
target_link_libraries(krakzncam_core PUBLIC
    ws2_32 d3d11 dxgi mfplat mfuuid wmcodecdspuuid windowscodecs ole32 shell32 advapi32 uuid winhttp bcrypt avrt)

# Oberfläche: Fenster mit eigener Titelleiste und KrakznCam-Design (Programm und Setup).
add_library(krakzncam_ui STATIC src/UiHost.cpp src/Theme.cpp)
target_link_libraries(krakzncam_ui PUBLIC imgui krakzncam_core dwmapi)

add_executable(krakzncam WIN32 src/main.cpp src/App.cpp res/krakzncam.rc)
target_link_libraries(krakzncam PRIVATE krakzncam_core krakzncam_ui d3dcompiler)

# Virtuelle Kamera (Media Source für den Frame-Server-Dienst von Windows 11).
add_library(krakzncam_vcam SHARED vcam/VirtualCamera.cpp vcam/Placeholder.cpp vcam/DirectFeed.cpp vcam/krakzncam_vcam.def)
target_include_directories(krakzncam_vcam PRIVATE src)
target_link_libraries(krakzncam_vcam PRIVATE krakzncam_core mfplat mfuuid advapi32 gdi32 ole32 avrt)

# Prüfwerkzeug für die virtuelle Kamera (liest Bilder wie OBS und speichert eins als PNG).
add_executable(krakzncam_vcamtest tools/vcamtest.cpp)
target_include_directories(krakzncam_vcamtest PRIVATE src)
target_link_libraries(krakzncam_vcamtest PRIVATE mfplat mfuuid mfreadwrite mf windowscodecs ole32)

# Setup (KrakznCam-Setup.exe): bringt Programm, Kamera-DLL und, falls angegeben, die iPhone-App mit.
set(KRAKZNCAM_IPA "" CACHE FILEPATH "iPhone-App (.ipa), die ins Setup gepackt wird (optional)")
set(KRAKZNCAM_PAYLOAD_RC "${CMAKE_BINARY_DIR}/payload/$<CONFIG>/payload.rc")
set(KRAKZNCAM_PAYLOAD "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/res/krakzncam.ico\"\n")
string(APPEND KRAKZNCAM_PAYLOAD "101 RCDATA \"$<TARGET_FILE:krakzncam>\"\n")
string(APPEND KRAKZNCAM_PAYLOAD "102 RCDATA \"$<TARGET_FILE:krakzncam_vcam>\"\n")
if(KRAKZNCAM_IPA AND EXISTS "${KRAKZNCAM_IPA}")
    file(TO_CMAKE_PATH "${KRAKZNCAM_IPA}" KRAKZNCAM_IPA_PATH)
    string(APPEND KRAKZNCAM_PAYLOAD "103 RCDATA \"${KRAKZNCAM_IPA_PATH}\"\n")
    message(STATUS "Setup enthält die iPhone-App: ${KRAKZNCAM_IPA_PATH}")
endif()
file(GENERATE OUTPUT "${KRAKZNCAM_PAYLOAD_RC}" CONTENT "${KRAKZNCAM_PAYLOAD}")
add_executable(krakzncam_setup WIN32 setup/SetupMain.cpp setup/Installer.cpp "${KRAKZNCAM_PAYLOAD_RC}")
target_include_directories(krakzncam_setup PRIVATE setup)
target_link_libraries(krakzncam_setup PRIVATE krakzncam_core krakzncam_ui)
add_dependencies(krakzncam_setup krakzncam krakzncam_vcam)
set_target_properties(krakzncam_setup PROPERTIES OUTPUT_NAME "KrakznCam-Setup")

add_executable(krakzncam_iconmaker tools/IconMaker.cpp)
target_link_libraries(krakzncam_iconmaker PRIVATE d2d1 windowscodecs ole32)

add_executable(krakzncam_fakephone tools/fakephone.cpp tools/TestScene.cpp)
target_link_libraries(krakzncam_fakephone PRIVATE krakzncam_core winmm d2d1 dwrite)
