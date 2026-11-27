#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

struct FrameInfo {
    UINT codedWidth = 0;
    UINT codedHeight = 0;
    UINT displayWidth = 0;
    UINT displayHeight = 0;
    uint64_t counter = 0;  // zählt jedes neue Bild hoch
    int64_t ptsMicros = 0;
};

// Hält eine eigene Kopie des zuletzt dekodierten Bildes (NV12-Textur). Der Empfangs-Thread
// schreibt hinein, Vorschau und später die virtuelle Kamera lesen daraus.
class FrameStore {
public:
    explicit FrameStore(ID3D11Device* device);

    void StoreFromTexture(ID3D11Texture2D* source, UINT subresource, UINT displayWidth, UINT displayHeight,
                          int64_t ptsMicros);
    void StoreFromMemory(const uint8_t* nv12, UINT stride, UINT codedWidth, UINT codedHeight, UINT displayWidth,
                         UINT displayHeight, int64_t ptsMicros);

    // Ruft fn(texture, info) auf, falls seit lastCounter ein neues Bild da ist.
    template <class Fn>
    bool WithLatest(uint64_t& lastCounter, Fn&& fn) {
        std::lock_guard lock(m_mutex);
        if (!m_texture || m_info.counter == lastCounter) return false;
        lastCounter = m_info.counter;
        fn(m_texture.Get(), m_info);
        return true;
    }

private:
    bool EnsureTexture(UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    std::mutex m_mutex;
    FrameInfo m_info;
};
