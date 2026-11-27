#include "TestScene.h"

#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Yuv {
    float y;
    float u;
    float v;
};

// BT.709, Videobereich (16–235), wie es das iPhone liefert.
Yuv ToYuv(float r, float g, float b) {
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {16.0f + 219.0f * luma / 255.0f, 128.0f + 224.0f * (-0.1146f * r - 0.3854f * g + 0.5f * b) / 255.0f,
            128.0f + 224.0f * (0.5f * r - 0.4542f * g - 0.0458f * b) / 255.0f};
}

uint8_t Clamp8(float value) {
    return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
}

void Blend(uint8_t& target, float value, float alpha) {
    target = Clamp8(target + (value - target) * alpha);
}

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// Farben aus dem App-Design.
const Yuv kAccent = ToYuv(46, 125, 246);
const Yuv kText2 = ToYuv(172, 172, 178);

}  // namespace

template <class Draw>
bool TestScene::Render(UINT width, UINT height, std::vector<uint8_t>& bgra, Draw&& draw) {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(m_d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
        return false;
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw(target.Get());
    if (FAILED(target->EndDraw())) return false;
    bgra.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

bool TestScene::Init(UINT width, UINT height, UINT fps) {
    m_width = width & ~1u;
    m_height = height & ~1u;
    m_fps = fps ? fps : 30;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf()))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic)))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf())))) {
        return false;
    }

    // Hintergrund einmal rendern und nach NV12 umrechnen.
    std::vector<uint8_t> bgra;
    if (!Render(m_width, m_height, bgra, [this](ID2D1RenderTarget* target) { DrawBackground(target); })) return false;
    m_background.resize(static_cast<size_t>(m_width) * m_height * 3 / 2);
    uint8_t* y = m_background.data();
    uint8_t* uv = y + static_cast<size_t>(m_width) * m_height;
    for (UINT row = 0; row < m_height; ++row) {
        for (UINT col = 0; col < m_width; ++col) {
            const uint8_t* p = &bgra[(static_cast<size_t>(row) * m_width + col) * 4];
            y[static_cast<size_t>(row) * m_width + col] = Clamp8(ToYuv(p[2], p[1], p[0]).y);
        }
    }
    for (UINT row = 0; row < m_height / 2; ++row) {
        for (UINT col = 0; col < m_width / 2; ++col) {
            float r = 0, g = 0, b = 0;
            for (UINT dy = 0; dy < 2; ++dy) {
                for (UINT dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = &bgra[(static_cast<size_t>(row * 2 + dy) * m_width + col * 2 + dx) * 4];
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            }
            const Yuv c = ToYuv(r / 4, g / 4, b / 4);
            uv[static_cast<size_t>(row) * m_width + col * 2] = Clamp8(c.u);
            uv[static_cast<size_t>(row) * m_width + col * 2 + 1] = Clamp8(c.v);
        }
    }

    // Schriftzug auf transparentem Grund rendern und als Maske ablegen.
    if (!Render(m_width, m_height, bgra, [this](ID2D1RenderTarget* target) { DrawTitle(target); })) return false;
    ExtractTitle(bgra);

    // Zeichenfläche für den Zeitcode.
    m_clockWidth = (m_width / 2) & ~1u;
    m_clockHeight = static_cast<UINT>(m_height * 0.06f) & ~1u;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_wic->CreateBitmap(m_clockWidth, m_clockHeight, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                   &m_clockBitmap)) ||
        FAILED(m_d2d->CreateWicBitmapRenderTarget(m_clockBitmap.Get(), properties, &m_clockTarget)) ||
        FAILED(m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, m_height * 0.026f, L"de-de",
                                          &m_clockFormat)) ||
        FAILED(m_clockTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &m_clockBrush))) {
        return false;
    }
    m_clockTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    m_clockFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_clockFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_clockPixels.resize(static_cast<size_t>(m_clockWidth) * m_clockHeight * 4);

    m_frame.resize(m_background.size());
    return true;
}

void TestScene::DrawBackground(ID2D1RenderTarget* target) {
    const float w = static_cast<float>(m_width);
    const float h = static_cast<float>(m_height);

    // Verlauf von oben nach unten, wie der Hintergrund der App.
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, Rgb(22, 22, 25)}, {0.7f, Rgb(11, 11, 13)}, {1.0f, Rgb(11, 11, 13)}};
    ComPtr<ID2D1GradientStopCollection> linearStops;
    ComPtr<ID2D1LinearGradientBrush> linear;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 3, &linearStops)) &&
        SUCCEEDED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, h)), linearStops.Get(), &linear))) {
        target->FillRectangle(D2D1::RectF(0, 0, w, h), linear.Get());
    }

    // Lichtschein oben in der Mitte.
    const D2D1_GRADIENT_STOP glow[] = {
        {0.0f, Rgb(46, 46, 52, 1.0f)}, {0.5f, Rgb(46, 46, 52, 0.5f)}, {1.0f, Rgb(46, 46, 52, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> glowStops;
    ComPtr<ID2D1RadialGradientBrush> radial;
    if (SUCCEEDED(target->CreateGradientStopCollection(glow, 3, &glowStops)) &&
        SUCCEEDED(target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(D2D1::Point2F(w * 0.5f, -h * 0.18f), D2D1::Point2F(0, 0), w * 0.63f,
                                                h * 0.49f),
            glowStops.Get(), &radial))) {
        target->FillRectangle(D2D1::RectF(0, 0, w, h), radial.Get());
    }

    // Eckmarken: zeigen, ob das Bild vollständig und unbeschnitten ankommt.
    ComPtr<ID2D1SolidColorBrush> marks;
    if (SUCCEEDED(target->CreateSolidColorBrush(Rgb(255, 255, 255, 0.22f), &marks))) {
        const float stroke = std::max(2.0f, h / 540.0f);
        const float inset = h * 0.04f;
        const float length = h * 0.045f;
        const float x[2] = {inset, w - inset};
        const float y[2] = {inset, h - inset};
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                const float dx = i == 0 ? length : -length;
                const float dy = j == 0 ? length : -length;
                target->DrawLine(D2D1::Point2F(x[i], y[j]), D2D1::Point2F(x[i] + dx, y[j]), marks.Get(), stroke);
                target->DrawLine(D2D1::Point2F(x[i], y[j]), D2D1::Point2F(x[i], y[j] + dy), marks.Get(), stroke);
            }
        }
    }
}

void TestScene::DrawTitle(ID2D1RenderTarget* target) {
    const float w = static_cast<float>(m_width);
    const float h = static_cast<float>(m_height);
    ComPtr<ID2D1SolidColorBrush> white;
    ComPtr<ID2D1SolidColorBrush> grey;
    ComPtr<ID2D1SolidColorBrush> dim;
    target->CreateSolidColorBrush(Rgb(244, 244, 246), &white);
    target->CreateSolidColorBrush(Rgb(172, 172, 178), &grey);
    target->CreateSolidColorBrush(Rgb(112, 112, 118), &dim);

    // „Krakzn“ weiß, „Cam“ grau – in dünner Schrift wie die Titel der App.
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextLayout> title;
    const wchar_t titleText[] = L"KrakznCam";
    if (SUCCEEDED(m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, h * 0.115f, L"de-de", &titleFormat)) &&
        SUCCEEDED(m_dwrite->CreateTextLayout(titleText, 9, titleFormat.Get(), w, h * 0.2f, &title))) {
        title->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        title->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        title->SetDrawingEffect(grey.Get(), DWRITE_TEXT_RANGE{6, 3});
        target->DrawTextLayout(D2D1::Point2F(0, h * 0.33f), title.Get(), white.Get());
    }

    // Kleine gesperrte Großbuchstaben darunter.
    ComPtr<IDWriteTextFormat> labelFormat;
    ComPtr<IDWriteTextLayout> label;
    const wchar_t labelText[] = L"TESTQUELLE";
    const float labelSize = h * 0.0175f;
    if (SUCCEEDED(m_dwrite->CreateTextFormat(L"Bahnschrift", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, labelSize, L"de-de",
                                             &labelFormat)) &&
        SUCCEEDED(m_dwrite->CreateTextLayout(labelText, 10, labelFormat.Get(), w, h * 0.05f, &label))) {
        label->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        ComPtr<IDWriteTextLayout1> spaced;
        if (SUCCEEDED(label.As(&spaced))) spaced->SetCharacterSpacing(0, labelSize * 0.24f, 0, DWRITE_TEXT_RANGE{0, 10});
        target->DrawTextLayout(D2D1::Point2F(labelSize * 0.12f, h * 0.535f), label.Get(), dim.Get());
    }
}

void TestScene::ExtractTitle(const std::vector<uint8_t>& bgra) {
    UINT minX = m_width, minY = m_height, maxX = 0, maxY = 0;
    for (UINT row = 0; row < m_height; ++row) {
        for (UINT col = 0; col < m_width; ++col) {
            if (bgra[(static_cast<size_t>(row) * m_width + col) * 4 + 3] == 0) continue;
            minX = std::min(minX, col);
            maxX = std::max(maxX, col);
            minY = std::min(minY, row);
            maxY = std::max(maxY, row);
        }
    }
    if (maxX < minX) return;
    m_titleX = minX & ~1u;
    m_titleY = minY & ~1u;
    m_titleWidth = std::min(m_width - m_titleX, ((maxX - m_titleX + 2) + 1) & ~1u);
    m_titleHeight = std::min(m_height - m_titleY, ((maxY - m_titleY + 2) + 1) & ~1u);
    m_titleLuma.assign(static_cast<size_t>(m_titleWidth) * m_titleHeight, 0);
    m_titleAlpha.assign(static_cast<size_t>(m_titleWidth) * m_titleHeight, 0);
    for (UINT row = 0; row < m_titleHeight; ++row) {
        for (UINT col = 0; col < m_titleWidth; ++col) {
            const uint8_t* p = &bgra[(static_cast<size_t>(m_titleY + row) * m_width + m_titleX + col) * 4];
            const size_t i = static_cast<size_t>(row) * m_titleWidth + col;
            m_titleAlpha[i] = p[3];
            if (p[3] > 0) {
                const float scale = 255.0f / p[3];  // vormultipliziert -> echte Farbe
                m_titleLuma[i] = Clamp8(ToYuv(p[2] * scale, p[1] * scale, p[0] * scale).y);
            }
        }
    }
}

const std::vector<uint8_t>& TestScene::Frame(int64_t index) {
    memcpy(m_frame.data(), m_background.data(), m_background.size());
    const double seconds = static_cast<double>(index) / m_fps;
    DrawGlow(seconds);
    CompositeTitle();
    DrawProgress(seconds);
    DrawClock(index);
    return m_frame;
}

void TestScene::DrawGlow(double seconds) {
    // Ein blauer Schein zieht langsam eine Ellipse um die Bildmitte (eine Runde in 9 s).
    const double angle = seconds * 2.0 * kPi / 9.0;
    const float cx = m_width * 0.5f + static_cast<float>(std::cos(angle)) * m_width * 0.2f;
    const float cy = m_height * 0.47f + static_cast<float>(std::sin(angle)) * m_height * 0.13f;
    const float radius = m_height * 0.36f;
    const float peak = 0.34f;
    const int x0 = std::max(0, static_cast<int>(cx - radius)) & ~1;
    const int x1 = std::min(static_cast<int>(m_width), static_cast<int>(cx + radius) + 2) & ~1;
    const int y0 = std::max(0, static_cast<int>(cy - radius)) & ~1;
    const int y1 = std::min(static_cast<int>(m_height), static_cast<int>(cy + radius) + 2) & ~1;
    const float inverse = 1.0f / (radius * radius);
    uint8_t* luma = m_frame.data();
    uint8_t* chroma = luma + static_cast<size_t>(m_width) * m_height;

    for (int row = y0; row < y1; ++row) {
        const float dy = row + 0.5f - cy;
        for (int col = x0; col < x1; ++col) {
            const float dx = col + 0.5f - cx;
            const float d = (dx * dx + dy * dy) * inverse;
            if (d >= 1.0f) continue;
            const float a = peak * (1.0f - d) * (1.0f - d);
            Blend(luma[static_cast<size_t>(row) * m_width + col], kAccent.y, a);
        }
    }
    for (int row = y0 / 2; row < y1 / 2; ++row) {
        const float dy = row * 2 + 1.0f - cy;
        for (int col = x0 / 2; col < x1 / 2; ++col) {
            const float dx = col * 2 + 1.0f - cx;
            const float d = (dx * dx + dy * dy) * inverse;
            if (d >= 1.0f) continue;
            const float a = peak * (1.0f - d) * (1.0f - d);
            uint8_t* uv = chroma + static_cast<size_t>(row) * m_width + col * 2;
            Blend(uv[0], kAccent.u, a);
            Blend(uv[1], kAccent.v, a);
        }
    }
}

void TestScene::CompositeTitle() {
    if (m_titleAlpha.empty()) return;
    uint8_t* luma = m_frame.data();
    uint8_t* chroma = luma + static_cast<size_t>(m_width) * m_height;
    for (UINT row = 0; row < m_titleHeight; ++row) {
        for (UINT col = 0; col < m_titleWidth; ++col) {
            const size_t i = static_cast<size_t>(row) * m_titleWidth + col;
            if (m_titleAlpha[i] == 0) continue;
            Blend(luma[static_cast<size_t>(m_titleY + row) * m_width + m_titleX + col], m_titleLuma[i],
                  m_titleAlpha[i] / 255.0f);
        }
    }
    // Die Schrift ist grau bis weiß: Farbe dort zur Mitte (128) ziehen.
    for (UINT row = 0; row < m_titleHeight / 2; ++row) {
        for (UINT col = 0; col < m_titleWidth / 2; ++col) {
            const size_t i = static_cast<size_t>(row * 2) * m_titleWidth + col * 2;
            const float a = (m_titleAlpha[i] + m_titleAlpha[i + 1] + m_titleAlpha[i + m_titleWidth] +
                             m_titleAlpha[i + m_titleWidth + 1]) /
                            (4.0f * 255.0f);
            if (a <= 0.0f) continue;
            uint8_t* uv = chroma + static_cast<size_t>(m_titleY / 2 + row) * m_width + m_titleX + col * 2;
            Blend(uv[0], 128.0f, a);
            Blend(uv[1], 128.0f, a);
        }
    }
}

void TestScene::DrawProgress(double seconds) {
    // Dünner Balken, der alle 2 s (ein Keyframe-Abstand) einmal volläuft.
    const float fraction = static_cast<float>(std::fmod(seconds, 2.0) / 2.0);
    const float barWidth = m_width * 0.16f;
    const float barHeight = std::max(4.0f, m_height * 0.0056f);
    const float radius = barHeight * 0.5f;
    const float left = (m_width - barWidth) * 0.5f;
    const float centerY = m_height * 0.79f;
    const float filled = left + barWidth * fraction;
    auto coverage = [&](float px, float py) {
        const float qx = std::clamp(px, left + radius, left + barWidth - radius);
        const float d = std::sqrt((px - qx) * (px - qx) + (py - centerY) * (py - centerY)) - radius;
        return std::clamp(0.5f - d, 0.0f, 1.0f);
    };
    const int x0 = std::max(0, static_cast<int>(left) - 2) & ~1;
    const int x1 = std::min(static_cast<int>(m_width), static_cast<int>(left + barWidth) + 4) & ~1;
    const int y0 = std::max(0, static_cast<int>(centerY - radius) - 2) & ~1;
    const int y1 = std::min(static_cast<int>(m_height), static_cast<int>(centerY + radius) + 4) & ~1;
    uint8_t* luma = m_frame.data();
    uint8_t* chroma = luma + static_cast<size_t>(m_width) * m_height;
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            const float px = col + 0.5f;
            const float cover = coverage(px, row + 0.5f);
            if (cover <= 0.0f) continue;
            uint8_t& y = luma[static_cast<size_t>(row) * m_width + col];
            const float track = y + (235.0f - y) * 0.12f;
            const float fill = std::clamp(filled - px + 0.5f, 0.0f, 1.0f);
            Blend(y, track + (kAccent.y - track) * fill, cover);
        }
    }
    for (int row = y0 / 2; row < y1 / 2; ++row) {
        for (int col = x0 / 2; col < x1 / 2; ++col) {
            const float px = col * 2 + 1.0f;
            const float cover = coverage(px, row * 2 + 1.0f);
            const float fill = std::clamp(filled - px + 0.5f, 0.0f, 1.0f);
            if (cover * fill <= 0.0f) continue;
            uint8_t* uv = chroma + static_cast<size_t>(row) * m_width + col * 2;
            Blend(uv[0], kAccent.u, cover * fill);
            Blend(uv[1], kAccent.v, cover * fill);
        }
    }
}

void TestScene::DrawClock(int64_t index) {
    if (!m_clockTarget) return;
    const int64_t hundredths = index * 100 / m_fps;
    wchar_t text[96];
    swprintf(text, 96, L"%02lld:%02lld:%02lld,%02lld   ·   Bild %lld", hundredths / 360000, hundredths / 6000 % 60,
             hundredths / 100 % 60, hundredths % 100, index);
    m_clockTarget->BeginDraw();
    m_clockTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
    m_clockTarget->DrawText(text, static_cast<UINT32>(wcslen(text)), m_clockFormat.Get(),
                            D2D1::RectF(0, 0, static_cast<float>(m_clockWidth), static_cast<float>(m_clockHeight)),
                            m_clockBrush.Get());
    if (FAILED(m_clockTarget->EndDraw())) return;
    if (FAILED(m_clockBitmap->CopyPixels(nullptr, m_clockWidth * 4, static_cast<UINT>(m_clockPixels.size()),
                                         m_clockPixels.data()))) {
        return;
    }
    const UINT left = ((m_width - m_clockWidth) / 2) & ~1u;
    const UINT top = static_cast<UINT>(m_height * 0.82f) & ~1u;
    if (top + m_clockHeight > m_height) return;
    uint8_t* luma = m_frame.data();
    for (UINT row = 0; row < m_clockHeight; ++row) {
        for (UINT col = 0; col < m_clockWidth; ++col) {
            const uint8_t alpha = m_clockPixels[(static_cast<size_t>(row) * m_clockWidth + col) * 4 + 3];
            if (alpha == 0) continue;
            Blend(luma[static_cast<size_t>(top + row) * m_width + left + col], kText2.y, alpha / 255.0f);
        }
    }
}
