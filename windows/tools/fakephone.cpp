// KabelCam-Testquelle: tut so, als wäre sie die iPhone-App (Protokoll: docs/protokoll.md).
// Kodiert ein bewegtes Testbild mit dem H.264-Encoder von Windows und sendet es auf 127.0.0.1.
//
//   kabelcam_fakephone.exe [--port 17878] [--width 1920] [--height 1080] [--fps 30]
//                          [--bitrate 12000000] [--seconds 0]
//
// Danach: kabelcam.exe --direct 127.0.0.1:17878

#include "Net.h"
#include "StreamProtocol.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <oaidl.h>
#include <strmif.h>
#include <timeapi.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    uint16_t port = 17878;
    UINT width = 1920;
    UINT height = 1080;
    UINT fps = 30;
    UINT bitrate = 12'000'000;
    double seconds = 0;  // 0 = unbegrenzt
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
};

bool ContainsSps(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1F) == 7) return true;
    }
    return false;
}

class Encoder {
public:
    bool Init(const Options& options) {
        m_options = options;
        HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_mft));
        if (FAILED(hr)) return Fail("H.264-Encoder nicht verfügbar", hr);

        ComPtr<IMFMediaType> output;
        MFCreateMediaType(&output);
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, options.bitrate);
        MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, options.width, options.height);
        MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, options.fps, 1);
        MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        hr = m_mft->SetOutputType(0, output.Get(), 0);
        if (FAILED(hr)) return Fail("Ausgabeformat abgelehnt", hr);

        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, options.width, options.height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, options.fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = m_mft->SetInputType(0, input.Get(), 0);
        if (FAILED(hr)) return Fail("Eingabeformat abgelehnt", hr);

        // Wie auf dem iPhone: konstante Rate, keine B-Frames, alle 2 s ein Keyframe, niedrige Latenz.
        if (SUCCEEDED(m_mft.As(&m_codec))) {
            SetUInt(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetUInt(CODECAPI_AVEncMPVGOPSize, options.fps * 2);
            SetUInt(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            VARIANT value{};
            value.vt = VT_BOOL;
            value.boolVal = VARIANT_TRUE;
            m_codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
        }

        MFT_OUTPUT_STREAM_INFO info{};
        m_mft->GetOutputStreamInfo(0, &info);
        m_outputSize = info.cbSize > 0 ? info.cbSize : options.width * options.height * 2;
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        ReadSequenceHeader();
        return true;
    }

    bool Encode(const std::vector<uint8_t>& nv12, int64_t index, bool forceKeyframe, std::vector<EncodedFrame>& out) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buffer))) return false;
        BYTE* target = nullptr;
        buffer->Lock(&target, nullptr, nullptr);
        memcpy(target, nv12.data(), nv12.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        const LONGLONG duration = 10'000'000LL / m_options.fps;
        sample->SetSampleTime(index * duration);
        sample->SetSampleDuration(duration);
        if (forceKeyframe) SetUInt(CODECAPI_AVEncVideoForceKeyFrame, 1);

        HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) return Fail("ProcessInput", hr);

        while (true) {
            ComPtr<IMFMediaBuffer> outBuffer;
            ComPtr<IMFSample> outSample;
            MFCreateMemoryBuffer(m_outputSize, &outBuffer);
            MFCreateSample(&outSample);
            outSample->AddBuffer(outBuffer.Get());
            MFT_OUTPUT_DATA_BUFFER output{};
            output.pSample = outSample.Get();
            DWORD status = 0;
            hr = m_mft->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> type;
                if (SUCCEEDED(m_mft->GetOutputAvailableType(0, 0, &type))) m_mft->SetOutputType(0, type.Get(), 0);
                ReadSequenceHeader();
                continue;
            }
            if (FAILED(hr)) return Fail("ProcessOutput", hr);

            UINT32 cleanPoint = 0;
            outSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);
            BYTE* bytes = nullptr;
            DWORD length = 0;
            outBuffer->Lock(&bytes, nullptr, &length);
            EncodedFrame frame;
            frame.data.assign(bytes, bytes + length);
            outBuffer->Unlock();
            frame.keyframe = cleanPoint != 0;
            if (frame.keyframe && !ContainsSps(frame.data)) {
                if (m_sequenceHeader.empty()) ReadSequenceHeader();
                frame.data.insert(frame.data.begin(), m_sequenceHeader.begin(), m_sequenceHeader.end());
            }
            out.push_back(std::move(frame));
        }
        return true;
    }

private:
    void SetUInt(const GUID& api, UINT value) {
        if (!m_codec) return;
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = value;
        m_codec->SetValue(&api, &v);
    }

    void ReadSequenceHeader() {
        ComPtr<IMFMediaType> current;
        if (FAILED(m_mft->GetOutputCurrentType(0, &current))) return;
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            m_sequenceHeader.resize(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_sequenceHeader.data(), size, nullptr);
        }
    }

    bool Fail(const char* what, HRESULT hr) {
        printf("Encoder-Fehler: %s (0x%08X)\n", what, static_cast<unsigned>(hr));
        return false;
    }

    Options m_options;
    ComPtr<IMFTransform> m_mft;
    ComPtr<ICodecAPI> m_codec;
    DWORD m_outputSize = 0;
    std::vector<uint8_t> m_sequenceHeader;
};

// Farbbalken (BT.709, Videobereich) mit einem wandernden weißen Block und Zählbalken oben.
class TestPattern {
public:
    TestPattern(UINT width, UINT height) : m_width(width), m_height(height) {
        static const uint8_t bars[8][3] = {
            {235, 128, 128}, {219, 16, 138}, {188, 154, 16}, {173, 42, 26},
            {78, 214, 230},  {63, 102, 240}, {32, 240, 118}, {16, 128, 128},
        };
        m_base.resize(static_cast<size_t>(width) * height * 3 / 2);
        uint8_t* y = m_base.data();
        uint8_t* uv = y + static_cast<size_t>(width) * height;
        for (UINT row = 0; row < height; ++row) {
            for (UINT col = 0; col < width; ++col) y[row * width + col] = bars[col * 8 / width][0];
        }
        for (UINT row = 0; row < height / 2; ++row) {
            for (UINT col = 0; col < width / 2; ++col) {
                const uint8_t* bar = bars[(col * 2) * 8 / width];
                uv[row * width + col * 2] = bar[1];
                uv[row * width + col * 2 + 1] = bar[2];
            }
        }
    }

    const std::vector<uint8_t>& Frame(int64_t index) {
        m_frame = m_base;
        uint8_t* y = m_frame.data();
        uint8_t* uv = y + static_cast<size_t>(m_width) * m_height;
        const UINT box = m_height / 5;
        const UINT x0 = static_cast<UINT>((index * (m_width / 120)) % (m_width - box)) & ~1u;
        const UINT y0 = (m_height - box - m_height / 10) & ~1u;
        for (UINT row = y0; row < y0 + box; ++row) memset(y + row * m_width + x0, 235, box);
        for (UINT row = y0 / 2; row < (y0 + box) / 2; ++row) memset(uv + row * m_width + x0, 128, box);
        // Oben: Bildzähler als 16 helle/dunkle Felder (zum Erkennen hängender Bilder).
        const UINT cell = m_width / 16;
        const UINT cellHeight = m_height / 20;
        for (UINT bit = 0; bit < 16; ++bit) {
            const uint8_t value = ((index >> (15 - bit)) & 1) ? 235 : 16;
            for (UINT row = 0; row < cellHeight; ++row) memset(y + row * m_width + bit * cell, value, cell - 4);
            for (UINT row = 0; row < cellHeight / 2; ++row) memset(uv + row * m_width + bit * cell, 128, cell - 4);
        }
        return m_frame;
    }

private:
    UINT m_width;
    UINT m_height;
    std::vector<uint8_t> m_base;
    std::vector<uint8_t> m_frame;
};

bool SendMessageTo(SOCKET s, uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> message = KabelCam::BuildMessage(type, payload.data(), payload.size());
    return SendAll(s, message.data(), message.size());
}

void ServeClient(SOCKET client, const Options& options) {
    // HELLO wie von der iPhone-App, mit einem Ablaufdatum in 5 Tagen zum Testen der Anzeige.
    const long long expires = static_cast<long long>(time(nullptr)) + 5 * 86400;
    char hello[512];
    snprintf(hello, sizeof(hello),
             "{\"version\":1,\"app\":\"KabelCam-Testquelle\",\"codec\":\"h264\",\"width\":%u,\"height\":%u,"
             "\"fps\":%u,\"bitrate\":%u,\"lens\":\"test\",\"signatureExpires\":%lld}",
             options.width, options.height, options.fps, options.bitrate, expires);
    std::vector<uint8_t> helloPayload(hello, hello + strlen(hello));
    if (!SendMessageTo(client, KabelCam::kMsgHello, helloPayload)) return;

    // Befehle vom PC (z. B. "keyframe") in einem eigenen Thread lesen.
    std::atomic<bool> forceKeyframe{true};
    std::atomic<bool> connected{true};
    std::thread reader([&] {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        while (KabelCam::ReadMessage(client, type, payload)) {
            std::string text(payload.begin(), payload.end());
            if (type == KabelCam::kMsgCommand && text.find("keyframe") != std::string::npos) {
                printf("Befehl erhalten: %s\n", text.c_str());
                forceKeyframe = true;
            }
        }
        connected = false;
    });

    Encoder encoder;
    TestPattern pattern(options.width, options.height);
    if (encoder.Init(options)) {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto frameDuration = std::chrono::microseconds(1'000'000 / options.fps);
        auto windowStart = start;
        uint64_t windowFrames = 0;
        uint64_t windowBytes = 0;
        std::vector<EncodedFrame> encoded;
        for (int64_t index = 0; connected; ++index) {
            encoded.clear();
            if (!encoder.Encode(pattern.Frame(index), index, forceKeyframe.exchange(false), encoded)) break;
            bool sendFailed = false;
            for (const EncodedFrame& frame : encoded) {
                std::vector<uint8_t> payload;
                payload.reserve(9 + frame.data.size());
                KabelCam::AppendBE64(payload, static_cast<uint64_t>(index * 1'000'000 / options.fps));
                payload.push_back(frame.keyframe ? 1 : 0);
                payload.insert(payload.end(), frame.data.begin(), frame.data.end());
                if (!SendMessageTo(client, KabelCam::kMsgVideoFrame, payload)) {
                    sendFailed = true;
                    break;
                }
                ++windowFrames;
                windowBytes += frame.data.size();
            }
            if (sendFailed) break;

            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed >= 1.0) {
                printf("%5.1f FPS  %6.2f Mbit/s\n", windowFrames / elapsed, windowBytes * 8.0 / elapsed / 1e6);
                windowStart = now;
                windowFrames = 0;
                windowBytes = 0;
            }
            if (options.seconds > 0 && std::chrono::duration<double>(now - start).count() >= options.seconds) break;
            std::this_thread::sleep_until(start + frameDuration * (index + 1));
        }
    }
    shutdown(client, SD_BOTH);
    reader.join();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::wstring key = argv[i];
        const wchar_t* value = argv[i + 1];
        if (key == L"--port") options.port = static_cast<uint16_t>(_wtoi(value));
        else if (key == L"--width") options.width = static_cast<UINT>(_wtoi(value)) & ~1u;
        else if (key == L"--height") options.height = static_cast<UINT>(_wtoi(value)) & ~1u;
        else if (key == L"--fps") options.fps = static_cast<UINT>(_wtoi(value));
        else if (key == L"--bitrate") options.bitrate = static_cast<UINT>(_wtoi(value));
        else if (key == L"--seconds") options.seconds = _wtof(value);
    }
    if (options.fps == 0) options.fps = 30;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    NetInit();
    timeBeginPeriod(1);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0) {
        printf("Port %u ist belegt.\n", options.port);
        return 1;
    }
    printf("Testquelle bereit auf 127.0.0.1:%u (%ux%u, %u FPS, %.1f Mbit/s)\n", options.port, options.width,
           options.height, options.fps, options.bitrate / 1e6);
    fflush(stdout);

    while (true) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        printf("PC verbunden\n");
        fflush(stdout);
        ServeClient(client, options);
        closesocket(client);
        printf("PC getrennt\n");
        fflush(stdout);
        if (options.seconds > 0) break;
    }
    closesocket(listener);
    timeEndPeriod(1);
    MFShutdown();
    CoUninitialize();
    return 0;
}
