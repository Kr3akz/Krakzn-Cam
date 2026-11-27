#include "UiHost.h"

#include "Log.h"
#include "TextUtil.h"
#include "Theme.h"

#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace {

const float kClearColor[4] = {11.0f / 255.0f, 11.0f / 255.0f, 13.0f / 255.0f, 1.0f};

// Stärke des (unsichtbaren) Fensterrahmens bei der Skalierung des Fensters.
int FrameThickness(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

}  // namespace

UiHost::~UiHost() {
    if (m_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (m_frameLatencyWaitable) CloseHandle(m_frameLatencyWaitable);
    m_renderTarget.Reset();
    m_swapChain.Reset();
    if (m_hwnd && IsWindow(m_hwnd)) DestroyWindow(m_hwnd);
    if (m_options.instance) UnregisterClassW(m_options.className, m_options.instance);
}

bool UiHost::Create(const Options& options) {
    m_options = options;
    ImGui_ImplWin32_EnableDpiAwareness();
    if (!CreateDevice()) {
        MessageBoxW(nullptr, L"Die Grafik konnte nicht gestartet werden.", options.title, MB_ICONERROR);
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = StaticWndProc;
    windowClass.hInstance = options.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(options.instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(options.instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    windowClass.hbrBackground = CreateSolidBrush(RGB(11, 11, 13));  // kein weißes Aufblitzen beim Vergrößern
    windowClass.lpszClassName = options.className;
    RegisterClassExW(&windowClass);

    m_dpiScale = options.exactPixelSize
                     ? 1.0f
                     : ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const int width = options.exactPixelSize ? options.width : static_cast<int>(options.width * m_dpiScale);
    const int height = options.exactPixelSize ? options.height : static_cast<int>(options.height * m_dpiScale);
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!options.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    // Mittig auf dem Hauptbildschirm.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + std::max(0, static_cast<int>((work.right - work.left - width) / 2));
    const int y = work.top + std::max(0, static_cast<int>((work.bottom - work.top - height) / 2));
    m_hwnd = CreateWindowExW(0, options.className, options.title, style, x, y, width, height, nullptr, nullptr,
                             options.instance, this);
    if (!m_hwnd || !CreateSwapChain()) {
        MessageBoxW(nullptr, L"Das Fenster konnte nicht erstellt werden.", options.title, MB_ICONERROR);
        return false;
    }

    // Dunkler Rahmen (Windows 11), abgerundete Ecken; danach den neuen Rahmen übernehmen.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF border = RGB(44, 44, 48);
    DwmSetWindowAttribute(m_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    const int corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style2 = ImGui::GetStyle();
    style2.WindowPadding = ImVec2(0.0f, 0.0f);
    style2.WindowBorderSize = 0.0f;
    style2.ChildBorderSize = 0.0f;
    // Alles Sichtbare zeichnet Theme selbst; ImGui liefert Fenster, Eingabe und Schrift.
    Theme::SetScale(m_dpiScale);
    Theme::LoadFonts();
    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
    m_imguiReady = true;
    return true;
}

void UiHost::Show(int showCommand) {
    ShowWindow(m_hwnd, showCommand);
    UpdateWindow(m_hwnd);
}

bool UiHost::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (m_options.videoSupport) flags |= D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &m_device, &level, &m_context);
    if (FAILED(hr)) {
        LogLine("Grafikkarte nicht nutzbar (0x%08X), weiche auf WARP aus", static_cast<unsigned>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &level, &m_context);
        if (FAILED(hr)) return false;
    }
    // Decoder (Empfangs-Thread), virtuelle Kamera und Oberfläche teilen sich das Gerät.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        LogLine("Grafik: %s (Feature-Level 0x%X)", Utf8(desc.Description).c_str(), static_cast<unsigned>(level));
    }
    return true;
}

bool UiHost::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    return SUCCEEDED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTarget));
}

bool UiHost::CreateSwapChain() {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
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
    // Mit Warte-Objekt: Das Warten auf den Bildschirm passiert vor dem Zeichnen und ohne die
    // Grafikkarte zu sperren. Sonst hält Present() sie bis zu 16 ms fest, und Decoder und
    // virtuelle Kamera müssen so lange warten.
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        desc.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr, &m_swapChain);
    }
    if (FAILED(hr)) {
        desc.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr, &m_swapChain);
    }
    if (FAILED(hr)) {
        LogLine("Swapchain konnte nicht erstellt werden (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    m_swapChainFlags = desc.Flags;
    ComPtr<IDXGISwapChain2> swapChain2;
    if (m_swapChainFlags && SUCCEEDED(m_swapChain.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
    }
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    return CreateRenderTarget();
}

bool UiHost::ProcessMessages(bool* hadInput) {
    bool running = true;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) running = false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (hadInput) *hadInput = true;
    }
    return running;
}

bool UiHost::ReadyForFrame(DWORD timeoutMs) {
    // Nimmt der Bildschirm gerade ein neues Bild an? Nur dann zeichnen, sonst würde Present()
    // blockieren und dabei die Grafikkarte für Decoder und virtuelle Kamera sperren.
    // (Liegt das Fenster hinter einem Spiel, nimmt Windows Bilder nur noch zögerlich ab.)
    return !m_frameLatencyWaitable || WaitForSingleObjectEx(m_frameLatencyWaitable, timeoutMs, TRUE) == WAIT_OBJECT_0;
}

bool UiHost::Occluded() {
    if (!m_occluded) return false;
    // Ab und zu nachsehen, ob das Fenster wieder sichtbar ist (zeichnet nichts).
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastOcclusionTest >= 250) {
        m_lastOcclusionTest = now;
        if (m_swapChain->Present(0, DXGI_PRESENT_TEST) == S_OK) m_occluded = false;
    }
    return m_occluded;
}

void UiHost::BeginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (!pendingInput.empty()) {
        pendingInput.front()(ImGui::GetIO());
        pendingInput.erase(pendingInput.begin());
    }
    ImGui::NewFrame();
    m_nextHotRects.clear();
    m_nextCaptionHeight = 0.0f;
}

void UiHost::EndFrame(bool present, bool vsync) {
    ImGui::Render();
    m_context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_renderTarget.Get(), kClearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Kopfzeile dieses Bildes gilt ab jetzt für die Treffer-Prüfung des Fensters.
    m_hotRects.swap(m_nextHotRects);
    m_captionHeight = m_nextCaptionHeight;
    if (present) {
        const HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
        // Ganz verdeckt (z. B. hinter einem Spiel im Vollbild): nicht weiter zeichnen.
        if (hr == DXGI_STATUS_OCCLUDED) {
            m_occluded = true;
            m_lastOcclusionTest = GetTickCount64();
        }
    }
}

void UiHost::AddCaptionHotRect(ImVec2 min, ImVec2 max) {
    m_nextHotRects.push_back(RECT{static_cast<LONG>(min.x), static_cast<LONG>(min.y), static_cast<LONG>(max.x + 1.0f),
                                  static_cast<LONG>(max.y + 1.0f)});
}

bool UiHost::InHotRect(POINT point) const {
    for (const RECT& rect : m_hotRects) {
        if (PtInRect(&rect, point)) return true;
    }
    return false;
}

float UiHost::DrawWindowButtons(float right, float centerY) {
    using namespace Theme;
    struct WindowButton {
        const char* id;
        char32_t glyph;
        bool close;
    };
    const bool maximized = IsZoomed(m_hwnd) != FALSE;
    std::vector<WindowButton> buttons = {{"##win_minimize", 0xE921, false}};
    if (m_options.resizable) buttons.push_back({"##win_maximize", maximized ? 0xE923u : 0xE922u, false});
    buttons.push_back({"##win_close", 0xE8BB, true});

    const ImVec2 size(Px(42.0f), Px(32.0f));
    const float gap = Px(2.0f);
    const float total = size.x * buttons.size() + gap * (buttons.size() - 1);
    float x = right - total;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const WindowButton& button : buttons) {
        const ImVec2 min(x, centerY - size.y * 0.5f);
        ImGui::SetCursorScreenPos(min);
        const bool clicked = ImGui::InvisibleButton(button.id, size);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const float hover = Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 20.0f);
        // Schließen wird rot wie überall in Windows, die anderen hellen sich nur auf.
        const ImU32 background = button.close ? WithAlpha(IM_COL32(196, 43, 28, 255), hover * (held ? 0.8f : 1.0f))
                                              : White((held ? 0.05f : 0.09f) * hover);
        if (hover > 0.01f) dl->AddRectFilled(min, min + size, A(background), Px(8.0f));
        const ImU32 glyph = button.close ? Mix(kText2, IM_COL32_WHITE, hover) : Mix(kText2, kText, hover);
        DrawIcon(dl, button.glyph, min + size * 0.5f, Px(10.5f), glyph);
        AddCaptionHotRect(min, min + size);
        if (clicked) {
            if (button.glyph == 0xE921) ShowWindow(m_hwnd, SW_MINIMIZE);
            else if (button.close) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            else ShowWindow(m_hwnd, maximized ? SW_RESTORE : SW_MAXIMIZE);
        }
        x += size.x + gap;
    }
    return total;
}

LRESULT CALLBACK UiHost::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* host = reinterpret_cast<UiHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return host ? host->HandleMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT UiHost::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) {
            // Standard-Rahmen berechnen lassen, dann die Titelleiste oben wieder dazunehmen.
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const LONG top = params->rgrc[0].top;
            const LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            params->rgrc[0].top = top;
            // Maximiert ragt das Fenster um die Rahmenstärke über den Bildschirm hinaus.
            if (IsZoomed(hwnd)) params->rgrc[0].top += FrameThickness(hwnd);
            return result;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT) return hit;  // Ränder links, rechts und unten
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        if (m_options.resizable && !IsZoomed(hwnd)) {
            const int border = FrameThickness(hwnd);
            if (point.y < border) {
                RECT client{};
                GetClientRect(hwnd, &client);
                if (point.x < border * 2) return HTTOPLEFT;
                if (point.x >= client.right - border * 2) return HTTOPRIGHT;
                return HTTOP;
            }
        }
        if (point.y < static_cast<LONG>(m_captionHeight) && !InHotRect(point)) return HTCAPTION;
        return HTCLIENT;
    }
    default: break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
    switch (msg) {
    case WM_SIZE:
        m_minimized = wParam == SIZE_MINIMIZED;
        if (m_swapChain && !m_minimized) {
            m_renderTarget.Reset();
            m_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, m_swapChainFlags);
            CreateRenderTarget();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && onActivated) onActivated();
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const float scale = GetDpiForWindow(hwnd) / 96.0f;
        info->ptMinTrackSize.x = static_cast<LONG>(m_options.minWidth * scale);
        info->ptMinTrackSize.y = static_cast<LONG>(m_options.minHeight * scale);
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
        if ((wParam & 0xfff0) == SC_CLOSE && canClose && !canClose()) return 0;
        break;
    case WM_CLOSE:
        if (canClose && !canClose()) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
