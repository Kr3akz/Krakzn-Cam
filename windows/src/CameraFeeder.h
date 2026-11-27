#pragma once
#include "FrameStore.h"
#include "VCamShared.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>

// Versorgt die virtuelle Kamera mit Bildern: rechnet das neueste Bild aus der Bildablage auf
// der Grafikkarte ins gewünschte Format (NV12) um und schreibt es in den gemeinsamen Speicher
// (VCamShared.h). Arbeitet nur, solange ein Programm die Kamera benutzt.
//
// Benutzt von KrakznCam selbst und von der Kamera-DLL, wenn KrakznCam nicht läuft.
class CameraFeeder {
public:
    struct Consumer {
        bool active = false;  // ein Programm benutzt die Kamera gerade
        UINT width = 0;
        UINT height = 0;
        UINT fps = 0;
    };

    CameraFeeder(ID3D11Device* device, FrameStore& store);
    ~CameraFeeder();
    CameraFeeder(const CameraFeeder&) = delete;
    CameraFeeder& operator=(const CameraFeeder&) = delete;

    // Bei jedem neuen Bild (und ab und zu ohne) aufrufen.
    Consumer Feed();
    void CloseShared();

private:
    bool OpenShared();
    bool ConfigureProcessor(ID3D11Texture2D* input, const FrameInfo& info, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    FrameStore& m_store;

    HANDLE m_mapping = nullptr;
    VCam::Header* m_header = nullptr;
    HANDLE m_frameReadyEvent = nullptr;  // „neues Bild“ an die Kamera
    ULONGLONG m_lastOpenAttempt = 0;
    ULONGLONG m_lastEventAttempt = 0;
    uint64_t m_lastCounter = 0;
    std::chrono::steady_clock::time_point m_lastWriteTime;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
    // Zwei Zwischenpuffer im Wechsel: Die Grafikkarte rechnet das neue Bild, während das vorige,
    // längst fertige ausgelesen wird. So muss nie auf die Grafikkarte gewartet werden.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging[2];
    Microsoft::WRL::ComPtr<ID3D11Query> m_copyDone[2];
    bool m_stagingFull[2] = {false, false};
    int m_stagingNext = 0;
    ID3D11Texture2D* m_configuredInput = nullptr;
    FrameInfo m_configuredInfo;
    UINT m_outputWidth = 0;
    UINT m_outputHeight = 0;
    ULONGLONG m_processorRetryAt = 0;  // nach einem Fehler später erneut versuchen

    // Statistik fürs Protokoll (alle 10 s).
    std::chrono::steady_clock::time_point m_statStart;
    uint32_t m_statFrames = 0;
    double m_statTotalMs = 0.0;
    double m_statMaxMs = 0.0;
    double m_statMaxGapMs = 0.0;
};
