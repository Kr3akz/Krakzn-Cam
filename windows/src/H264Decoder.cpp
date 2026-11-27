#include "H264Decoder.h"

#include "Log.h"

#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

bool H264Decoder::Initialize(ID3D11Device* device) {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_decoder));
    if (FAILED(hr)) {
        LogLine("H.264-Decoder nicht verfügbar (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(m_decoder->GetAttributes(&attributes))) {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);  // Bilder sofort ausgeben, nicht puffern
        UINT32 d3d11Aware = 0;
        if (device && SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) && d3d11Aware) {
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&m_resetToken, &m_deviceManager)) &&
                SUCCEEDED(m_deviceManager->ResetDevice(device, m_resetToken)) &&
                SUCCEEDED(m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                    reinterpret_cast<ULONG_PTR>(m_deviceManager.Get())))) {
                m_hardware = true;
            } else {
                m_deviceManager.Reset();
            }
        }
    }

    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m_decoder->SetInputType(0, input.Get(), 0);
    if (FAILED(hr)) {
        LogLine("H.264-Eingabeformat abgelehnt (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    if (!ConfigureOutput()) return false;

    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    LogLine("H.264-Decoder bereit (%s)", m_hardware ? "Grafikkarte" : "Software");
    return true;
}

bool H264Decoder::ConfigureOutput() {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        if (FAILED(m_decoder->GetOutputAvailableType(0, index, &type))) break;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        if (FAILED(m_decoder->SetOutputType(0, type.Get(), 0))) continue;

        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &m_codedWidth, &m_codedHeight);
        m_displayWidth = m_codedWidth;
        m_displayHeight = m_codedHeight;
        // H.264 kodiert 1080p als 1088 Zeilen; der sichtbare Bereich steht in der Apertur.
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                    nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            m_displayWidth = static_cast<UINT32>(area.Area.cx);
            m_displayHeight = static_cast<UINT32>(area.Area.cy);
        }
        UINT32 stride = 0;
        m_defaultStride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride)
                                                                                     : static_cast<LONG>(m_codedWidth);

        MFT_OUTPUT_STREAM_INFO info{};
        m_decoder->GetOutputStreamInfo(0, &info);
        m_providesSamples =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outputSize = info.cbSize;
        m_softwareSample.Reset();
        return true;
    }
    LogLine("Decoder bietet kein NV12-Ausgabeformat an");
    return false;
}

bool H264Decoder::Decode(const uint8_t* data, size_t size, int64_t ptsMicros, const FrameCallback& onFrame) {
    if (!m_decoder || size == 0) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) return false;
    BYTE* target = nullptr;
    if (FAILED(buffer->Lock(&target, nullptr, nullptr))) return false;
    memcpy(target, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsMicros * 10);  // Media Foundation rechnet in 100 ns

    HRESULT hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        if (!Drain(onFrame)) return false;
        hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        LogLine("Decoder: ProcessInput fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    return Drain(onFrame);
}

bool H264Decoder::Drain(const FrameCallback& onFrame) {
    for (int guard = 0; guard < 16; ++guard) {
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        if (!m_providesSamples) {
            if (!m_softwareSample) {
                DWORD size = m_outputSize ? m_outputSize : m_codedWidth * m_codedHeight * 3 / 2;
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(MFCreateSample(&m_softwareSample))) {
                    return false;
                }
                m_softwareSample->AddBuffer(buffer.Get());
            }
            output.pSample = m_softwareSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = m_decoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (m_providesSamples && output.pSample) output.pSample->Release();
            if (!ConfigureOutput()) return false;
            LogLine("Videoformat: %ux%u (sichtbar %ux%u)", m_codedWidth, m_codedHeight, m_displayWidth,
                    m_displayHeight);
            continue;
        }
        if (FAILED(hr)) {
            LogLine("Decoder: ProcessOutput fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
            return false;
        }
        if (output.pSample) {
            Deliver(output.pSample, onFrame);
            if (m_providesSamples) output.pSample->Release();
        }
    }
    return true;
}

void H264Decoder::Deliver(IMFSample* sample, const FrameCallback& onFrame) {
    DecodedFrame frame;
    frame.codedWidth = m_codedWidth;
    frame.codedHeight = m_codedHeight;
    frame.displayWidth = m_displayWidth;
    frame.displayHeight = m_displayHeight;
    LONGLONG time = 0;
    sample->GetSampleTime(&time);
    frame.ptsMicros = time / 10;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;

    // Hardware-Pfad: das Bild liegt bereits als Textur auf der Grafikkarte.
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (SUCCEEDED(buffer.As(&dxgiBuffer))) {
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) {
            dxgiBuffer->GetSubresourceIndex(&frame.subresource);
            frame.texture = texture.Get();
            onFrame(frame);
        }
        return;
    }

    // Software-Pfad: NV12 im Arbeitsspeicher.
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scanline, &pitch))) {
            frame.data = scanline;
            frame.stride = static_cast<UINT>(pitch > 0 ? pitch : -pitch);
            onFrame(frame);
            buffer2d->Unlock2D();
        }
        return;
    }
    BYTE* bytes = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
        frame.data = bytes;
        frame.stride = static_cast<UINT>(std::abs(m_defaultStride));
        onFrame(frame);
        buffer->Unlock();
    }
}
