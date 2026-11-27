#pragma once
#include "FrameStore.h"

#include <d3d11.h>
#include <wrl/client.h>

// Wandelt das neueste NV12-Bild mit dem Video-Prozessor der Grafikkarte in ein
// BGRA-Bild für die Vorschau um (inklusive Zuschnitt 1088 -> 1080 und BT.709-Farben).
class VideoView {
public:
    bool Init(ID3D11Device* device);
    // true, wenn ein neues Bild umgerechnet wurde.
    bool Update(FrameStore& store);

    bool HasFrame() const { return m_hasFrame; }
    ID3D11ShaderResourceView* Srv() const { return m_srv.Get(); }
    ID3D11Texture2D* OutputTexture() const { return m_output.Get(); }
    UINT Width() const { return m_width; }
    UINT Height() const { return m_height; }

private:
    bool Configure(ID3D11Texture2D* input, const FrameInfo& info);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    ID3D11Texture2D* m_configuredInput = nullptr;
    UINT m_codedWidth = 0;
    UINT m_codedHeight = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    uint64_t m_lastCounter = 0;
    bool m_hasFrame = false;
    bool m_failed = false;
};
