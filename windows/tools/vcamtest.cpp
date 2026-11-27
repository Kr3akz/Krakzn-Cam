// Prüft die virtuelle Kamera, ohne OBS:
//
//   krakzncam_vcamtest.exe [--device] [--frames 90] [--width 1280] [--height 720] [--fps 30] [--out bild.png]
//
// Ohne --device wird krakzncam_vcam.dll direkt in diesen Prozess geladen (keine Einrichtung nötig).
// Mit --device wird die angemeldete Kamera „KrakznCam“ über Windows geöffnet, also genau so wie
// von OBS (KrakznCam muss laufen und die Kamera eingerichtet sein).
// Danach werden Bilder gelesen, die Bildrate gemessen und das letzte Bild als PNG gespeichert.

#include "VCamShared.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    bool device = false;
    int frames = 90;
    UINT32 width = 1280;
    UINT32 height = 720;
    UINT32 fps = 30;
    std::wstring out = L"vcamtest.png";
};

int Fail(const char* what, HRESULT hr) {
    printf("FEHLER: %s (0x%08X)\n", what, static_cast<unsigned>(hr));
    return 1;
}

HRESULT OpenInProcess(ComPtr<IMFMediaSource>& source) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dll(path);
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + VCam::kDllName;
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    if (!getClassObject) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject(VCam::kSourceClsid, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IMFActivate> activate;
    hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate));
    if (FAILED(hr)) return hr;
    return activate->ActivateObject(IID_PPV_ARGS(&source));
}

HRESULT OpenDevice(ComPtr<IMFMediaSource>& source) {
    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attributes.Get(), &devices, &count);
    if (FAILED(hr)) return hr;
    hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    printf("Kameras:\n");
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr;
        UINT32 length = 0;
        devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length);
        printf("  %ls\n", name ? name : L"?");
        // Windows hängt bei virtuellen Kameras einen Zusatz an („KrakznCam (Windows virtuelle Kamera)“).
        if (name && wcsncmp(name, VCam::kFriendlyName, wcslen(VCam::kFriendlyName)) == 0 && !source) {
            hr = devices[i]->ActivateObject(IID_PPV_ARGS(&source));
        }
        CoTaskMemFree(name);
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return hr;
}

uint8_t Clamp(float value) {
    return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
}

// NV12 (BT.709, Videobereich) nach BGRA und als PNG speichern.
bool SavePng(const std::vector<uint8_t>& nv12, UINT32 width, UINT32 height, const std::wstring& path) {
    std::vector<uint8_t> bgra(size_t(width) * height * 4);
    const uint8_t* uv = nv12.data() + size_t(width) * height;
    for (UINT32 y = 0; y < height; ++y) {
        for (UINT32 x = 0; x < width; ++x) {
            const float Y = (nv12[size_t(y) * width + x] - 16.0f) * 255.0f / 219.0f;
            const size_t c = size_t(y / 2) * width + (x & ~1u);
            const float U = (uv[c] - 128.0f) * 255.0f / 224.0f;
            const float V = (uv[c + 1] - 128.0f) * 255.0f / 224.0f;
            uint8_t* p = &bgra[(size_t(y) * width + x) * 4];
            p[2] = Clamp(Y + 1.5748f * V);
            p[1] = Clamp(Y - 0.1873f * U - 0.4681f * V);
            p[0] = Clamp(Y + 1.8556f * U);
            p[3] = 255;
        }
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data());
    frame->Commit();
    return SUCCEEDED(encoder->Commit());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&] { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--device") options.device = true;
        else if (arg == L"--frames") options.frames = _wtoi(next().c_str());
        else if (arg == L"--width") options.width = static_cast<UINT32>(_wtoi(next().c_str()));
        else if (arg == L"--height") options.height = static_cast<UINT32>(_wtoi(next().c_str()));
        else if (arg == L"--fps") options.fps = static_cast<UINT32>(_wtoi(next().c_str()));
        else if (arg == L"--out") options.out = next();
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    ComPtr<IMFMediaSource> source;
    HRESULT hr = options.device ? OpenDevice(source) : OpenInProcess(source);
    if (FAILED(hr) || !source) return Fail(options.device ? "Kamera „KrakznCam“ nicht gefunden" : "DLL nicht ladbar", hr);
    printf("Quelle geöffnet (%s)\n", options.device ? "über Windows" : "direkt");

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    if (FAILED(hr)) return Fail("SourceReader", hr);

    // Passendes Format suchen und alle angebotenen auflisten.
    ComPtr<IMFMediaType> chosen;
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), index, &type))) break;
        UINT32 w = 0, h = 0, num = 0, den = 1;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
        printf("  Format %lu: %ux%u @ %u FPS\n", index, w, h, den ? num / den : 0);
        if (w == options.width && h == options.height && den && num / den == options.fps) chosen = type;
    }
    if (!chosen) return Fail("Gewünschtes Format nicht angeboten", E_INVALIDARG);
    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, chosen.Get());
    if (FAILED(hr)) return Fail("Format setzen", hr);

    using Clock = std::chrono::steady_clock;
    std::vector<uint8_t> last;
    int received = 0;
    Clock::time_point first;
    Clock::time_point lastTime;
    for (int i = 0; i < options.frames * 3 && received < options.frames; ++i) {
        DWORD stream = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &timestamp, &sample);
        if (FAILED(hr)) return Fail("ReadSample", hr);
        if (!sample) continue;
        lastTime = Clock::now();
        if (received == 0) first = lastTime;
        ++received;
        ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            last.assign(data, data + length);
            buffer->Unlock();
        }
    }
    const double seconds = std::chrono::duration<double>(lastTime - first).count();
    printf("%d Bilder gelesen, %.1f FPS\n", received, received > 1 && seconds > 0 ? (received - 1) / seconds : 0.0);
    if (last.size() >= VCam::FrameBytes(options.width, options.height)) {
        printf("Letztes Bild: %s\n", SavePng(last, options.width, options.height, options.out) ? "gespeichert" : "Fehler");
    }
    reader.Reset();
    source->Shutdown();
    source.Reset();
    MFShutdown();
    CoUninitialize();
    return received >= options.frames ? 0 : 2;
}
