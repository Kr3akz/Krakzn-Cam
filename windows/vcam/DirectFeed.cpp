#include "DirectFeed.h"

#include "CameraFeeder.h"
#include "FrameStore.h"
#include "Log.h"
#include "Net.h"
#include "StreamClient.h"
#include "VCamShared.h"

#include <d3d11_4.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <cstdlib>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// Grafikkarte für Dekodieren und Umrechnen. Im Kameradienst gibt es keinen Bildschirm, die
// Grafikkarte lässt sich trotzdem nutzen; notfalls rechnet WARP auf dem Prozessor.
ComPtr<ID3D11Device> CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) {
        LogLine("Grafikkarte nicht nutzbar (0x%08X), weiche auf WARP aus", static_cast<unsigned>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (FAILED(hr)) return nullptr;
    }
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    return device;
}

std::wstring LogPath() {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramData%\\KrakznCam\\kamera-direkt.log", path, MAX_PATH)) return {};
    return path;
}

}  // namespace

DirectFeed::DirectFeed() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

DirectFeed::~DirectFeed() {
    Stop();
    CloseHandle(m_stopEvent);
}

void DirectFeed::Start() {
    if (m_thread.joinable()) return;
    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { Run(); });
}

void DirectFeed::Stop() {
    if (!m_thread.joinable()) return;
    SetEvent(m_stopEvent);
    m_thread.join();
}

void DirectFeed::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    NetInit();
    LogInit(LogPath());
    LogLine("Kamera empfängt bei Bedarf selbst (KrakznCam %s)", KRAKZNCAM_VERSION);

    ComPtr<ID3D11Device> device = CreateDevice();
    if (!device) {
        LogLine("Keine Grafik verfügbar, die Kamera zeigt nur den Hinweis");
        WaitForSingleObject(m_stopEvent, INFINITE);
    } else {
        FrameStore store(device.Get());
        HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CameraFeeder feeder(device.Get(), store);
        {
            StreamClient client(device.Get(), store, frameEvent);
            // Zum Testen ohne iPhone: KRAKZNCAM_VCAM_DIRECT=127.0.0.1:17878 (Testquelle).
            std::string host;
            uint16_t port = 0;
            char direct[64];
            size_t length = 0;
            if (getenv_s(&length, direct, sizeof(direct), "KRAKZNCAM_VCAM_DIRECT") == 0 && length > 1) {
                const std::string value(direct);
                const size_t colon = value.rfind(':');
                if (colon != std::string::npos) {
                    host = value.substr(0, colon);
                    port = static_cast<uint16_t>(atoi(value.c_str() + colon + 1));
                }
            }
            bool connected = false;
            ULONGLONG lastCheck = 0;
            bool appRunning = false;
            const HANDLE events[] = {m_stopEvent, frameEvent};
            for (;;) {
                const DWORD result = WaitForMultipleObjects(2, events, FALSE, 200);
                if (result == WAIT_OBJECT_0) break;
                const ULONGLONG now = GetTickCount64();
                if (now - lastCheck >= 1000 || lastCheck == 0) {
                    lastCheck = now;
                    appRunning = VCam::AppRunning();
                }
                if (appRunning && connected) {
                    LogLine("KrakznCam läuft: Kamera überlässt KrakznCam das iPhone");
                    client.Stop();
                    connected = false;
                } else if (!appRunning && !connected) {
                    LogLine("KrakznCam läuft nicht: Kamera verbindet sich selbst mit dem iPhone");
                    client.Start(host, port);
                    connected = true;
                }
                if (connected) feeder.Feed();
            }
            client.Stop();
        }
        feeder.CloseShared();
        CloseHandle(frameEvent);
    }
    LogLine("Kamera empfängt nicht mehr selbst");
    LogInit(L"");
    WSACleanup();
    MFShutdown();
    CoUninitialize();
}
