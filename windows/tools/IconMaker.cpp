// Zeichnet das KrakznCam-Logo (Webcam-Linse mit Standfuß) als Vektorgrafik mit Direct2D und
// schreibt daraus die App-Symbole:
//   krakzncam_iconmaker.exe <Zielordner>
//     ios-1024.png        iPhone-App-Symbol (quadratisch, ohne Transparenz; iOS rundet selbst ab)
//     krakzncam.ico       Windows-Symbol (16–256 px, abgerundetes Quadrat)
//     preview-<n>.png     Vorschau einzelner Windows-Größen

#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IWICImagingFactory> g_wic;

ComPtr<ID2D1GradientStopCollection> Stops(ID2D1RenderTarget* target, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
    ComPtr<ID2D1GradientStopCollection> collection;
    std::vector<D2D1_GRADIENT_STOP> list(stops);
    target->CreateGradientStopCollection(list.data(), static_cast<UINT32>(list.size()), &collection);
    return collection;
}

// Alle Maße beziehen sich auf eine 1024er-Zeichenfläche und werden mit `s` skaliert.
// rounded: abgerundetes Quadrat mit etwas Rand (Windows); sonst randlos (iOS maskiert selbst).
void DrawLogo(ID2D1RenderTarget* target, float size, bool rounded) {
    const float s = size / 1024.0f;
    target->Clear(D2D1::ColorF(0, 0, 0, 0));

    // Grundfläche: dunkler Verlauf wie der App-Hintergrund, oben ein sanfter Lichtschein.
    const float inset = rounded ? 40.0f * s : 0.0f;
    const D2D1_RECT_F area = D2D1::RectF(inset, inset, size - inset, size - inset);
    const float radius = rounded ? 210.0f * s : 0.0f;
    ComPtr<ID2D1LinearGradientBrush> background;
    target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, area.top), D2D1::Point2F(0, area.bottom)),
        Stops(target, {{0.0f, Rgb(40, 40, 46)}, {0.55f, Rgb(20, 20, 24)}, {1.0f, Rgb(11, 11, 13)}}).Get(), &background);
    ComPtr<ID2D1RadialGradientBrush> glow;
    target->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(size * 0.5f, area.top - 80.0f * s), D2D1::Point2F(0, 0),
                                            620.0f * s, 520.0f * s),
        Stops(target, {{0.0f, Rgb(70, 72, 84, 0.75f)}, {1.0f, Rgb(70, 72, 84, 0.0f)}}).Get(), &glow);
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(area, radius, radius);
    target->FillRoundedRectangle(shape, background.Get());
    target->FillRoundedRectangle(shape, glow.Get());

    const D2D1_POINT_2F center = D2D1::Point2F(512.0f * s, 432.0f * s);

    // Standfuß: schmaler Hals und abgerundeter Sockel, leicht grau, damit die Linse führt.
    ComPtr<ID2D1LinearGradientBrush> stand;
    target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 660.0f * s), D2D1::Point2F(0, 850.0f * s)),
        Stops(target, {{0.0f, Rgb(214, 216, 224)}, {1.0f, Rgb(160, 162, 172)}}).Get(), &stand);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(482.0f * s, 660.0f * s, 542.0f * s, 800.0f * s), 30.0f * s, 30.0f * s),
        stand.Get());
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(338.0f * s, 772.0f * s, 686.0f * s, 836.0f * s), 32.0f * s, 32.0f * s),
        stand.Get());

    // Linse: weißer Ring, blaue Iris (Akzentfarbe), dunkle Pupille, zwei Lichtpunkte.
    ComPtr<ID2D1SolidColorBrush> shadow;
    target->CreateSolidColorBrush(Rgb(0, 0, 0, 0.35f), &shadow);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x, center.y + 18.0f * s), 286.0f * s, 286.0f * s),
                        shadow.Get());
    ComPtr<ID2D1LinearGradientBrush> ring;
    target->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 146.0f * s), D2D1::Point2F(0, 718.0f * s)),
        Stops(target, {{0.0f, Rgb(250, 250, 252)}, {1.0f, Rgb(206, 208, 216)}}).Get(), &ring);
    target->FillEllipse(D2D1::Ellipse(center, 280.0f * s, 280.0f * s), ring.Get());

    ComPtr<ID2D1SolidColorBrush> gap;
    target->CreateSolidColorBrush(Rgb(14, 14, 18), &gap);
    target->FillEllipse(D2D1::Ellipse(center, 226.0f * s, 226.0f * s), gap.Get());

    ComPtr<ID2D1RadialGradientBrush> iris;
    target->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(center.x - 60.0f * s, center.y - 70.0f * s),
                                            D2D1::Point2F(0, 0), 300.0f * s, 300.0f * s),
        Stops(target, {{0.0f, Rgb(120, 176, 255)}, {0.45f, Rgb(46, 125, 246)}, {1.0f, Rgb(16, 52, 132)}}).Get(),
        &iris);
    target->FillEllipse(D2D1::Ellipse(center, 196.0f * s, 196.0f * s), iris.Get());

    ComPtr<ID2D1RadialGradientBrush> pupil;
    target->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), 92.0f * s, 92.0f * s),
        Stops(target, {{0.0f, Rgb(6, 8, 14)}, {0.85f, Rgb(10, 14, 26)}, {1.0f, Rgb(18, 36, 80)}}).Get(), &pupil);
    target->FillEllipse(D2D1::Ellipse(center, 92.0f * s, 92.0f * s), pupil.Get());

    ComPtr<ID2D1SolidColorBrush> glint;
    target->CreateSolidColorBrush(Rgb(255, 255, 255, 0.92f), &glint);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x - 74.0f * s, center.y - 78.0f * s), 34.0f * s, 34.0f * s),
                        glint.Get());
    glint->SetOpacity(0.45f);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + 62.0f * s, center.y + 58.0f * s), 14.0f * s, 14.0f * s),
                        glint.Get());
}

// Rendert das Logo und gibt PNG-Bytes zurück. opaque: Hintergrund statt Transparenz (für iOS).
std::vector<uint8_t> RenderPng(UINT size, bool rounded, bool opaque) {
    std::vector<uint8_t> png;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
        return png;
    }
    target->BeginDraw();
    DrawLogo(target.Get(), static_cast<float>(size), rounded);
    if (FAILED(target->EndDraw())) return png;

    // In einen Speicher-Stream als PNG kodieren (vormultipliziert -> gerade Deckkraft).
    ComPtr<IWICFormatConverter> converter;
    g_wic->CreateFormatConverter(&converter);
    converter->Initialize(bitmap.Get(), opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    ComPtr<IStream> stream;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(size, size);
    WICPixelFormatGUID format = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    frame->WriteSource(converter.Get(), nullptr);
    frame->Commit();
    encoder->Commit();

    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
    return png;
}

bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    return true;
}

// ICO mit PNG-Einträgen (seit Windows Vista für alle Größen erlaubt).
std::vector<uint8_t> BuildIco(const std::vector<std::pair<UINT, std::vector<uint8_t>>>& images) {
    std::vector<uint8_t> ico;
    auto put16 = [&](uint16_t v) { ico.push_back(v & 0xFF); ico.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) ico.push_back((v >> (8 * i)) & 0xFF); };
    put16(0);
    put16(1);
    put16(static_cast<uint16_t>(images.size()));
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
    for (const auto& [size, png] : images) {
        ico.push_back(size >= 256 ? 0 : static_cast<uint8_t>(size));
        ico.push_back(size >= 256 ? 0 : static_cast<uint8_t>(size));
        ico.push_back(0);
        ico.push_back(0);
        put16(1);
        put16(32);
        put32(static_cast<uint32_t>(png.size()));
        put32(offset);
        offset += static_cast<uint32_t>(png.size());
    }
    for (const auto& image : images) ico.insert(ico.end(), image.second.begin(), image.second.end());
    return ico;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring folder = argc > 1 ? argv[1] : L".";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf())) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
        printf("Direct2D/WIC nicht verfügbar\n");
        return 1;
    }
    CreateDirectoryW(folder.c_str(), nullptr);

    const std::vector<uint8_t> ios = RenderPng(1024, false, true);
    SaveFile(folder + L"\\ios-1024.png", ios);

    std::vector<std::pair<UINT, std::vector<uint8_t>>> images;
    for (UINT size : {16u, 20u, 24u, 32u, 40u, 48u, 64u, 96u, 128u, 256u}) {
        images.emplace_back(size, RenderPng(size, true, false));
    }
    SaveFile(folder + L"\\krakzncam.ico", BuildIco(images));
    for (UINT size : {32u, 256u}) {
        SaveFile(folder + L"\\preview-" + std::to_wstring(size) + L".png", RenderPng(size, true, false));
    }
    printf("Symbole geschrieben nach %ls\n", folder.c_str());
    CoUninitialize();
    return 0;
}
