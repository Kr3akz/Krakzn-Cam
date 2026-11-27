#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

// Testbild der Testquelle im Stil der App: dunkler Verlauf mit Lichtschein, Schriftzug
// „KrakznCam“, ein langsam wandernder blauer Schein, Zeitcode und ein Fortschrittsbalken,
// der alle zwei Sekunden (ein Keyframe-Abstand) einmal durchläuft.
// Liefert NV12 (BT.709, Videobereich) wie die Kamera des iPhones.
class TestScene {
public:
    bool Init(UINT width, UINT height, UINT fps);
    const std::vector<uint8_t>& Frame(int64_t index);

private:
    template <class Draw>
    bool Render(UINT width, UINT height, std::vector<uint8_t>& bgra, Draw&& draw);
    void DrawBackground(ID2D1RenderTarget* target);
    void DrawTitle(ID2D1RenderTarget* target);
    void ExtractTitle(const std::vector<uint8_t>& bgra);
    void DrawGlow(double seconds);
    void CompositeTitle();
    void DrawProgress(double seconds);
    void DrawClock(int64_t index);

    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_fps = 30;
    std::vector<uint8_t> m_background;  // NV12, einmal gerendert
    std::vector<uint8_t> m_frame;       // NV12, jedes Bild neu

    // Schriftzug als Maske: Helligkeit und Deckkraft im Rechteck um den Text.
    std::vector<uint8_t> m_titleLuma;
    std::vector<uint8_t> m_titleAlpha;
    UINT m_titleX = 0;
    UINT m_titleY = 0;
    UINT m_titleWidth = 0;
    UINT m_titleHeight = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2d;
    Microsoft::WRL::ComPtr<IWICImagingFactory> m_wic;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwrite;

    // Zeitcode: kleine Zeichenfläche, die jedes Bild neu beschrieben wird.
    Microsoft::WRL::ComPtr<IWICBitmap> m_clockBitmap;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> m_clockTarget;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_clockFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_clockBrush;
    std::vector<uint8_t> m_clockPixels;
    UINT m_clockWidth = 0;
    UINT m_clockHeight = 0;
};
