// KabelCam für Windows: empfängt das iPhone-Bild per USB und zeigt es an.
//
// Aufruf:
//   kabelcam.exe                               normaler Start
//   kabelcam.exe --direct 127.0.0.1:17878      Testquelle statt iPhone (kabelcam_fakephone.exe)
//   kabelcam.exe --check --log datei.txt       nur Einrichtung prüfen und ins Protokoll schreiben
//   kabelcam.exe --selftest --frames 60 --snapshot bild.png --screenshot ui.png --log datei.txt
//                                              unsichtbar laufen, bis N Bilder dekodiert sind, dann speichern

#include "App.h"
#include "FrameStore.h"
#include "Log.h"
#include "Net.h"
#include "SetupCheck.h"
#include "Snapshot.h"
#include "StreamClient.h"
#include "TextUtil.h"
#include "Theme.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdlib>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string directHost;
    uint16_t directPort = 0;
    std::wstring logPath;
    bool check = false;
    bool selftest = false;
    int frames = 60;        // Selbsttest: so viele Bilder abwarten (0 = nur Zeit ablaufen lassen)
    double timeout = 30.0;  // Selbsttest: höchstens so viele Sekunden
    App::Page page = App::Page::Camera;
    std::wstring snapshotPath;
    std::wstring screenshotPath;
};

struct Graphics {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;
};

Graphics g_gfx;
App* g_app = nullptr;
bool g_minimized = false;
const float kClearColor[4] = {11.0f / 255.0f, 11.0f / 255.0f, 13.0f / 255.0f, 1.0f};

Options ParseOptions() {
    Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--direct") {
            std::string value = Utf8(next());
            size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                options.directHost = value.substr(0, colon);
                options.directPort = static_cast<uint16_t>(atoi(value.c_str() + colon + 1));
            }
        } else if (arg == L"--log") {
            options.logPath = next();
        } else if (arg == L"--check") {
            options.check = true;
        } else if (arg == L"--selftest") {
            options.selftest = true;
        } else if (arg == L"--frames") {
            options.frames = _wtoi(next().c_str());
        } else if (arg == L"--timeout") {
            options.timeout = _wtof(next().c_str());
        } else if (arg == L"--page") {
            options.page = next() == L"setup" ? App::Page::Setup : App::Page::Camera;
        } else if (arg == L"--snapshot") {
            options.snapshotPath = next();
        } else if (arg == L"--screenshot") {
            options.screenshotPath = next();
        }
    }
    LocalFree(argv);
    return options;
}

bool CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_gfx.device, &level, &g_gfx.context);
    if (FAILED(hr)) {
        LogLine("Grafikkarte nicht nutzbar (0x%08X), weiche auf WARP aus", static_cast<unsigned>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &g_gfx.device, &level, &g_gfx.context);
        if (FAILED(hr)) return false;
    }
    // Decoder (Empfangs-Thread) und Oberfläche teilen sich das Gerät.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(g_gfx.device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(g_gfx.device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        LogLine("Grafik: %s (Feature-Level 0x%X)", Utf8(desc.Description).c_str(), static_cast<unsigned>(level));
    }
    return true;
}

bool CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_gfx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    return SUCCEEDED(g_gfx.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_gfx.renderTarget));
}

bool CreateSwapChain(HWND hwnd) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_gfx.device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_NONE;
    HRESULT hr = factory->CreateSwapChainForHwnd(g_gfx.device.Get(), hwnd, &desc, nullptr, nullptr, &g_gfx.swapChain);
    if (FAILED(hr)) {
        desc.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForHwnd(g_gfx.device.Get(), hwnd, &desc, nullptr, nullptr, &g_gfx.swapChain);
    }
    if (FAILED(hr)) {
        LogLine("Swapchain konnte nicht erstellt werden (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return CreateRenderTarget();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        g_minimized = wParam == SIZE_MINIMIZED;
        if (g_gfx.swapChain && !g_minimized) {
            g_gfx.renderTarget.Reset();
            g_gfx.swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && g_app) g_app->OnActivated();
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 900;
        info->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Alt-Menü abschalten
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SetupImGui(HWND hwnd, float scale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;

    // Alles Sichtbare zeichnet Theme selbst; ImGui liefert Fenster, Eingabe und Schrift.
    Theme::SetScale(scale);
    Theme::LoadFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_gfx.device.Get(), g_gfx.context.Get());
}

// Dunkle Titelleiste in der Farbe des Hintergrunds (Windows 11; ältere Versionen ignorieren das).
void StyleTitleBar(HWND hwnd) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(22, 22, 25);
    const COLORREF text = RGB(244, 244, 246);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}

void RenderFrame(App& app) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.Draw();
    ImGui::Render();
    g_gfx.context->OMSetRenderTargets(1, g_gfx.renderTarget.GetAddressOf(), nullptr);
    g_gfx.context->ClearRenderTargetView(g_gfx.renderTarget.Get(), kClearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// Normale Schleife. Gezeichnet wird nur bei neuen Bildern, Eingaben oder alle 250 ms für den
// Status. Ist das Fenster minimiert, wird gar nicht gezeichnet.
int RunLoop(App& app, HANDLE frameEvent) {
    int extraFrames = 2;
    bool running = true;
    while (running) {
        DWORD timeout = extraFrames > 0 || app.Animating() ? 0 : 250;
        MsgWaitForMultipleObjectsEx(1, &frameEvent, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            extraFrames = 2;  // ImGui braucht nach Eingaben noch ein, zwei Bilder
        }
        if (!running) break;
        if (g_minimized) {
            extraFrames = 0;
            continue;
        }
        app.UpdateVideo();
        RenderFrame(app);
        g_gfx.swapChain->Present(1, 0);
        if (extraFrames > 0) --extraFrames;
    }
    return 0;
}

int RunSelftest(App& app, StreamClient& client, HANDLE frameEvent, const Options& options) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(options.timeout * 1000.0));
    bool success = options.frames <= 0;  // ohne Bildziel: nur die Zeit ablaufen lassen
    while (Clock::now() < deadline) {
        WaitForSingleObject(frameEvent, 100);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        app.UpdateVideo();
        RenderFrame(app);
        g_gfx.swapChain->Present(0, 0);
        if (options.frames > 0 && static_cast<int>(client.Status().framesDecoded) >= options.frames &&
            app.Video().HasFrame()) {
            success = true;
            break;
        }
    }
    for (int i = 0; i < 40; ++i) {  // Animationen auslaufen lassen (Seite einblenden, Hintergrund)
        Sleep(16);
        app.UpdateVideo();
        RenderFrame(app);
        g_gfx.swapChain->Present(0, 0);
    }
    app.UpdateVideo();
    RenderFrame(app);
    if (!options.screenshotPath.empty()) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(g_gfx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            SaveTexturePng(g_gfx.device.Get(), g_gfx.context.Get(), backBuffer.Get(), options.screenshotPath);
        }
    }
    if (!options.snapshotPath.empty() && app.Video().HasFrame()) {
        SaveTexturePng(g_gfx.device.Get(), g_gfx.context.Get(), app.Video().OutputTexture(), options.snapshotPath);
    }

    const StreamStatus status = client.Status();
    LogLine("SELBSTTEST %s: %llu Bilder dekodiert, %.1f FPS, %.2f Mbit/s, Decoder: %s, Vorschau %ux%u, HELLO %dx%d",
            success ? "OK" : "FEHLGESCHLAGEN", static_cast<unsigned long long>(status.framesDecoded),
            status.measuredFps, status.measuredMbps, status.hardwareDecoding ? "Grafikkarte" : "Software",
            app.Video().Width(), app.Video().Height(), status.width, status.height);
    return success ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const Options options = ParseOptions();
    LogInit(options.logPath.empty() ? DefaultLogPath() : options.logPath);
    LogLine("KabelCam %s startet", KABELCAM_VERSION);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    NetInit();

    if (options.check) {
        const SetupReport report = RunSetupCheck();
        for (const SetupItem& item : report.items) {
            LogLine("[%s] %s: %s%s%s", SetupStatusName(item.status), item.title.c_str(), item.detail.c_str(),
                    item.actionLabel.empty() ? "" : "  -> ", Utf8(item.actionTarget).c_str());
        }
        LogLine("Bereit für Sideloadly: %s", report.sideloadReady ? "ja" : "nein");
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    if (!CreateDevice()) {
        MessageBoxW(nullptr, L"Die Grafik konnte nicht gestartet werden.", L"KabelCam", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(RGB(11, 11, 13));  // kein weißes Aufblitzen beim Vergrößern
    windowClass.lpszClassName = L"KabelCamWindow";
    RegisterClassExW(&windowClass);

    float scale = options.selftest
                      ? 1.0f
                      : ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const int width = options.selftest ? 1280 : static_cast<int>(1200 * scale);
    const int height = options.selftest ? 760 : static_cast<int>(740 * scale);
    HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"KabelCam", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, width, height, nullptr, nullptr, instance, nullptr);
    if (!hwnd || !CreateSwapChain(hwnd)) {
        MessageBoxW(nullptr, L"Das Fenster konnte nicht erstellt werden.", L"KabelCam", MB_ICONERROR);
        return 1;
    }
    StyleTitleBar(hwnd);
    SetupImGui(hwnd, scale);

    int exitCode = 0;
    {
        FrameStore store(g_gfx.device.Get());
        HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        StreamClient client(g_gfx.device.Get(), store, frameEvent);
        client.Start(options.directHost, options.directPort);
        App app(g_gfx.device.Get(), client, store, options.page);
        g_app = &app;

        if (options.selftest) {
            exitCode = RunSelftest(app, client, frameEvent, options);
        } else {
            ShowWindow(hwnd, showCommand);
            UpdateWindow(hwnd);
            exitCode = RunLoop(app, frameEvent);
        }

        g_app = nullptr;
        client.Stop();
        CloseHandle(frameEvent);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, instance);
    MFShutdown();
    CoUninitialize();
    LogLine("KabelCam beendet (Code %d)", exitCode);
    return exitCode;
}
