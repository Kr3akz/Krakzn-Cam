#include "Snapshot.h"

#include "Log.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

bool SaveTexturePng(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    const std::wstring& path) {
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        LogLine("PNG: nur BGRA-Texturen werden unterstützt (Format %d)", static_cast<int>(desc.Format));
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const UINT rowBytes = desc.Width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        uint8_t* row = pixels.data() + static_cast<size_t>(y) * rowBytes;
        memcpy(row, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        for (UINT x = 0; x < desc.Width; ++x) row[x * 4 + 3] = 255;  // undurchsichtig speichern
    }
    context->Unmap(staging.Get(), 0);

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(desc.Width, desc.Height);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(desc.Height, rowBytes, static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) {
        LogLine("PNG konnte nicht gespeichert werden (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}
