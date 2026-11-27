#include "FrameStore.h"

#include "Log.h"

#include <cstring>

namespace {

// Nicht jeder Grafiktreiber akzeptiert jede NV12-Textur als Eingang für den Video-Prozessor.
// Zuerst wie eine Decoder-Ausgabe (funktioniert fast überall), danach die Alternativen.
struct BindVariant {
    UINT flags;
    const char* name;
};
const BindVariant kBindVariants[] = {
    {D3D11_BIND_DECODER, "DECODER"},
    {D3D11_BIND_SHADER_RESOURCE, "SHADER_RESOURCE"},
    {D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, "RENDER_TARGET|SHADER_RESOURCE"},
    {D3D11_BIND_RENDER_TARGET, "RENDER_TARGET"},
    {0, "ohne"},
};
constexpr size_t kBindVariantCount = sizeof(kBindVariants) / sizeof(kBindVariants[0]);

}  // namespace

FrameStore::FrameStore(ID3D11Device* device) : m_device(device) {
    m_device->GetImmediateContext(&m_context);
}

bool FrameStore::EnsureTexture(UINT width, UINT height) {
    if (m_texture) {
        D3D11_TEXTURE2D_DESC current{};
        m_texture->GetDesc(&current);
        if (current.Width == width && current.Height == height) return true;
        m_texture.Reset();
        m_staging.Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    while (m_variant < kBindVariantCount) {
        desc.BindFlags = kBindVariants[m_variant].flags;
        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_texture);
        if (SUCCEEDED(hr)) {
            LogLine("NV12-Textur %ux%u angelegt (Bind: %s)", width, height, kBindVariants[m_variant].name);
            return true;
        }
        LogLine("NV12-Textur mit Bind %s nicht möglich (0x%08X)", kBindVariants[m_variant].name,
                static_cast<unsigned>(hr));
        ++m_variant;
    }
    return false;
}

bool FrameStore::TryNextTextureVariant() {
    std::lock_guard lock(m_mutex);
    ++m_variant;
    m_texture.Reset();
    m_staging.Reset();
    return m_variant < kBindVariantCount;
}

void FrameStore::StoreFromTexture(ID3D11Texture2D* source, UINT subresource, UINT displayWidth, UINT displayHeight,
                                  int64_t ptsMicros) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        LogLine("Unerwartetes Texturformat %d vom Decoder", static_cast<int>(desc.Format));
        return;
    }
    std::lock_guard lock(m_mutex);
    if (!EnsureTexture(desc.Width, desc.Height)) return;
    m_context->CopySubresourceRegion(m_texture.Get(), 0, 0, 0, 0, source, subresource, nullptr);
    m_info.codedWidth = desc.Width;
    m_info.codedHeight = desc.Height;
    m_info.displayWidth = displayWidth <= desc.Width ? displayWidth : desc.Width;
    m_info.displayHeight = displayHeight <= desc.Height ? displayHeight : desc.Height;
    m_info.ptsMicros = ptsMicros;
    ++m_info.counter;
}

void FrameStore::StoreFromMemory(const uint8_t* nv12, UINT stride, UINT codedWidth, UINT codedHeight,
                                 UINT displayWidth, UINT displayHeight, int64_t ptsMicros) {
    std::lock_guard lock(m_mutex);
    if (!EnsureTexture(codedWidth, codedHeight)) return;
    if (!m_staging) {
        D3D11_TEXTURE2D_DESC desc{};
        m_texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_staging))) {
            LogLine("NV12-Zwischentextur konnte nicht angelegt werden");
            return;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return;
    uint8_t* target = static_cast<uint8_t*>(mapped.pData);
    for (UINT y = 0; y < codedHeight; ++y) {
        memcpy(target + static_cast<size_t>(y) * mapped.RowPitch, nv12 + static_cast<size_t>(y) * stride, codedWidth);
    }
    // NV12: Die Farbebene (UV) folgt direkt auf die Helligkeitsebene (Y).
    uint8_t* targetUV = target + static_cast<size_t>(mapped.RowPitch) * codedHeight;
    const uint8_t* sourceUV = nv12 + static_cast<size_t>(stride) * codedHeight;
    for (UINT y = 0; y < codedHeight / 2; ++y) {
        memcpy(targetUV + static_cast<size_t>(y) * mapped.RowPitch, sourceUV + static_cast<size_t>(y) * stride,
               codedWidth);
    }
    m_context->Unmap(m_staging.Get(), 0);
    m_context->CopyResource(m_texture.Get(), m_staging.Get());

    m_info.codedWidth = codedWidth;
    m_info.codedHeight = codedHeight;
    m_info.displayWidth = displayWidth;
    m_info.displayHeight = displayHeight;
    m_info.ptsMicros = ptsMicros;
    ++m_info.counter;
}
