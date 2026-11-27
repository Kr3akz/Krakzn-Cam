#pragma once
// Fenster mit eigener Titelleiste, Direct3D 11 und Dear ImGui. Gemeinsam für KrakznCam und den
// Installer: Der App-Name, die Reiter und die Fensterknöpfe (Minimieren, Maximieren, Schließen)
// sitzen in derselben Kopfzeile, statt in einer getrennten Windows-Titelleiste darüber.
//
// So funktioniert der Rahmen:
// - WM_NCCALCSIZE nimmt die Windows-Titelleiste weg, die unsichtbaren Ränder zum Ziehen der
//   Fenstergröße bleiben. Schatten, abgerundete Ecken, Andocken und Animationen bleiben erhalten.
// - WM_NCHITTEST meldet die Kopfzeile als Titelleiste (Fenster ziehen, Doppelklick maximiert),
//   außer dort, wo Bedienelemente liegen. Die meldet der Zeichencode je Bild (AddCaptionHotRect).

#include "imgui.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <functional>
#include <vector>

class UiHost {
public:
    struct Options {
        HINSTANCE instance = nullptr;
        const wchar_t* className = L"KrakznCamWindow";
        const wchar_t* title = L"KrakznCam";
        int width = 1200;  // bei 100 % Bildschirm-Skalierung
        int height = 740;
        int minWidth = 900;
        int minHeight = 560;
        bool resizable = true;
        bool videoSupport = false;    // Gerät mit Video-Unterstützung (Hardware-Decoder)
        bool exactPixelSize = false;  // width/height genau so, ohne Skalierung (Selbsttest)
    };

    UiHost() = default;
    UiHost(const UiHost&) = delete;
    UiHost& operator=(const UiHost&) = delete;
    ~UiHost();

    bool Create(const Options& options);
    void Show(int showCommand);

    HWND Window() const { return m_hwnd; }
    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    IDXGISwapChain1* SwapChain() const { return m_swapChain.Get(); }
    float DpiScale() const { return m_dpiScale; }
    bool Minimized() const { return m_minimized; }

    // Arbeitet alle Fensternachrichten ab. false = Programm soll enden (WM_QUIT).
    // hadInput wird true, wenn Nachrichten da waren (ImGui braucht danach noch ein, zwei Bilder).
    bool ProcessMessages(bool* hadInput = nullptr);

    void BeginFrame();
    // Zeichnet das ImGui-Bild; present = anzeigen (vsync = mit dem Bildschirm synchron).
    void EndFrame(bool present, bool vsync = true);

    // --- Kopfzeile (je Bild nach BeginFrame aufrufen) ---------------------------------------
    void SetCaptionHeight(float height) { m_nextCaptionHeight = height; }
    void AddCaptionHotRect(ImVec2 min, ImVec2 max);
    // Fensterknöpfe rechts oben, senkrecht mittig auf centerY. Gibt die belegte Breite zurück.
    float DrawWindowButtons(float right, float centerY);

    // Selbsttest: je Bild eine simulierte Eingabe, nach der echten Win32-Eingabe eingespeist.
    std::vector<std::function<void(ImGuiIO&)>> pendingInput;
    // Fenster wurde aktiviert.
    std::function<void()> onActivated;
    // Darf das Fenster gerade geschlossen werden? (z. B. nicht mitten in einer Installation)
    std::function<bool()> canClose;

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreateRenderTarget();
    bool InHotRect(POINT point) const;

    Options m_options;
    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    UINT m_swapChainFlags = 0;
    HANDLE m_frameLatencyWaitable = nullptr;  // signalisiert: ein neues Bild darf gezeichnet werden
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
    float m_dpiScale = 1.0f;
    bool m_minimized = false;
    bool m_imguiReady = false;

    float m_captionHeight = 0.0f;
    float m_nextCaptionHeight = 0.0f;
    std::vector<RECT> m_hotRects;
    std::vector<RECT> m_nextHotRects;
};
