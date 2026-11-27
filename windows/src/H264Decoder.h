#pragma once
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

// Ein dekodiertes Bild (NV12). Entweder als Textur auf der Grafikkarte (Hardware-Pfad)
// oder als Speicherblock (Software-Pfad). Nur während des Callbacks gültig.
struct DecodedFrame {
    ID3D11Texture2D* texture = nullptr;
    UINT subresource = 0;
    const uint8_t* data = nullptr;
    UINT stride = 0;
    UINT codedWidth = 0;
    UINT codedHeight = 0;
    UINT displayWidth = 0;
    UINT displayHeight = 0;
    int64_t ptsMicros = 0;
};

// H.264-Decoder von Media Foundation. Mit D3D11-Gerät dekodiert er auf der Grafikkarte.
class H264Decoder {
public:
    using FrameCallback = std::function<void(const DecodedFrame&)>;

    bool Initialize(ID3D11Device* device);
    // Eine Annex-B-Zugriffseinheit (ein Frame) dekodieren.
    bool Decode(const uint8_t* data, size_t size, int64_t ptsMicros, const FrameCallback& onFrame);
    bool IsHardware() const { return m_hardware; }

private:
    bool ConfigureOutput();
    bool Drain(const FrameCallback& onFrame);
    void Deliver(IMFSample* sample, const FrameCallback& onFrame);

    Microsoft::WRL::ComPtr<IMFTransform> m_decoder;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    Microsoft::WRL::ComPtr<IMFSample> m_softwareSample;
    UINT m_resetToken = 0;
    bool m_hardware = false;
    bool m_providesSamples = false;
    DWORD m_outputSize = 0;
    UINT32 m_codedWidth = 0;
    UINT32 m_codedHeight = 0;
    UINT32 m_displayWidth = 0;
    UINT32 m_displayHeight = 0;
    LONG m_defaultStride = 0;
};
