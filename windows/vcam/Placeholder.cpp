#include "Placeholder.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct Yuv {
    float y, u, v;
};

// sRGB (0–255) nach BT.709 im Videobereich.
Yuv FromRgb(float r, float g, float b) {
    r /= 255.0f;
    g /= 255.0f;
    b /= 255.0f;
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {16.0f + 219.0f * y, 128.0f + 224.0f * (b - y) / 1.8556f, 128.0f + 224.0f * (r - y) / 1.5748f};
}

Yuv Mix(const Yuv& a, const Yuv& b, float t) {
    return {a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}

// Deckung eines Rings (Radius r, Stärke t) im Abstand d, mit einem Pixel Kantenglättung.
float Ring(float d, float r, float t) {
    return std::clamp(t * 0.5f + 0.5f - std::fabs(d - r), 0.0f, 1.0f);
}

float Disc(float d, float r) {
    return std::clamp(r + 0.5f - d, 0.0f, 1.0f);
}

// Hinweistext mit GDI als Graustufen-Maske (0–255) in Bildgröße.
std::vector<uint8_t> TextMask(uint32_t width, uint32_t height, int titleTop, std::vector<uint8_t>& secondary) {
    std::vector<uint8_t> mask(size_t(width) * height, 0);
    secondary.assign(size_t(width) * height, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return mask;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);  // von oben nach unten
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        return mask;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    auto draw = [&](const wchar_t* text, int size, int weight, const wchar_t* face, int top, std::vector<uint8_t>& out) {
        memset(bits, 0, size_t(width) * height * 4);
        HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT rect{0, top, static_cast<LONG>(width), static_cast<LONG>(height)};
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        SelectObject(dc, oldFont);
        DeleteObject(font);
        const uint8_t* pixels = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < out.size(); ++i) out[i] = pixels[i * 4 + 1];  // Grünkanal = Deckung
    };
    const int titleSize = std::max(12, static_cast<int>(height * 0.044f));
    const int hintSize = std::max(10, static_cast<int>(height * 0.024f));
    draw(L"Kein Bild von KrakznCam", titleSize, FW_LIGHT, L"Segoe UI Light", titleTop, mask);
    draw(L"Starte KrakznCam und öffne die App auf dem iPhone.", hintSize, FW_NORMAL, L"Segoe UI",
         titleTop + static_cast<int>(titleSize * 1.55f), secondary);

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return mask;
}

}  // namespace

const std::vector<uint8_t>& Placeholder::Get(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height && !m_data.empty()) return m_data;
    m_width = width;
    m_height = height;
    m_data.assign(size_t(width) * height * 3 / 2, 0);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float cx = w * 0.5f;
    const float cy = h * 0.40f;
    const float outer = h * 0.095f;          // weißer Rahmen
    const float outerWidth = outer * 0.17f;
    const float glass = outer - outerWidth * 0.5f;
    const float blue = outer * 0.52f;        // blauer Ring
    const float blueWidth = outer * 0.075f;

    const Yuv top = FromRgb(20, 20, 23);
    const Yuv bottom = FromRgb(11, 11, 13);
    const Yuv glassColor = FromRgb(9, 12, 22);
    const Yuv white = FromRgb(236, 236, 240);
    const Yuv accent = FromRgb(46, 125, 246);
    const Yuv text = FromRgb(244, 244, 246);
    const Yuv hint = FromRgb(140, 140, 146);

    std::vector<uint8_t> hintMask;
    const std::vector<uint8_t> titleMask =
        TextMask(width, height, static_cast<int>(cy + outer + h * 0.075f), hintMask);

    auto colorAt = [&](float x, float y, size_t index) {
        Yuv c = Mix(top, bottom, std::clamp(y / h, 0.0f, 1.0f));
        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        c = Mix(c, glassColor, Disc(d, glass));
        c = Mix(c, accent, Ring(d, blue, blueWidth));
        c = Mix(c, white, Ring(d, outer, outerWidth));
        if (index != SIZE_MAX) {
            c = Mix(c, text, titleMask[index] / 255.0f);
            c = Mix(c, hint, hintMask[index] / 255.0f);
        }
        return c;
    };

    uint8_t* luma = m_data.data();
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t index = size_t(y) * width + x;
            luma[index] = static_cast<uint8_t>(std::clamp(colorAt(x + 0.5f, y + 0.5f, index).y + 0.5f, 16.0f, 235.0f));
        }
    }
    // Farbe je 2×2-Block aus der Blockmitte (Text bleibt dabei farblos).
    uint8_t* chroma = m_data.data() + size_t(width) * height;
    for (uint32_t y = 0; y < height / 2; ++y) {
        for (uint32_t x = 0; x < width / 2; ++x) {
            const Yuv c = colorAt(x * 2.0f + 1.0f, y * 2.0f + 1.0f, SIZE_MAX);
            chroma[size_t(y) * width + x * 2] = static_cast<uint8_t>(std::clamp(c.u + 0.5f, 16.0f, 240.0f));
            chroma[size_t(y) * width + x * 2 + 1] = static_cast<uint8_t>(std::clamp(c.v + 0.5f, 16.0f, 240.0f));
        }
    }
    return m_data;
}
