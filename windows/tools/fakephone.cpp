// KrakznCam-Testquelle: tut so, als wäre sie die iPhone-App (Protokoll: docs/protokoll.md).
// Kodiert ein bewegtes Testbild im Stil der App (TestScene) mit dem H.264-Encoder von Windows
// und sendet es auf 127.0.0.1.
//
//   krakzncam_fakephone.exe [--port 17878] [--width 1920] [--height 1080] [--fps 30]
//                          [--bitrate 12000000] [--seconds 0]
//
// Danach: krakzncam.exe --direct 127.0.0.1:17878

#include "Json.h"
#include "Net.h"
#include "StreamProtocol.h"
#include "TestScene.h"

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
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

std::mutex g_sendMutex;  // Bilder und Zustand kommen aus zwei Threads

bool SendMessageTo(SOCKET s, uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> message = KrakznCam::BuildMessage(type, payload.data(), payload.size());
    std::lock_guard lock(g_sendMutex);
    return SendAll(s, message.data(), message.size());
}

// Ausgedachte Kamera mit denselben Schlüsseln wie die iPhone-App (STATE, siehe docs/protokoll.md).
// Die Automatik regelt sichtbar nach, und Helligkeit, Weißabgleich und Spiegeln wirken aufs Testbild.
class FakeCamera {
public:
    explicit FakeCamera(UINT fps) {
        auto number = [&](const char* key, double value) { Set(key, JsonValue{JsonValue::Type::Number, value}); };
        auto flag = [&](const char* key, bool value) {
            JsonValue v;
            v.type = JsonValue::Type::Bool;
            v.boolean = value;
            Set(key, v);
        };
        auto text = [&](const char* key, const char* value) {
            JsonValue v;
            v.type = JsonValue::Type::String;
            v.text = value;
            Set(key, v);
        };
        text("lens", "wide");
        text("lenses", "wide,ultraWide,telephoto,front");
        text("resolution", "hd1080");
        number("fps", fps);
        text("quality", "high");
        number("zoom", 1.0);
        number("zoom.max", 10.0);
        flag("exposure.auto", true);
        flag("exposure.manual", true);
        number("iso", 125);
        number("iso.min", 32);
        number("iso.max", 3072);
        number("shutter", 1.0 / 60.0);
        number("shutter.min", 1.0 / 32000.0);
        number("shutter.max", 1.0 / fps);
        number("ev", 0.0);
        number("ev.min", -3.0);
        number("ev.max", 3.0);
        flag("wb.auto", true);
        flag("wb.manual", true);
        number("wb.temp", 5200);
        number("wb.tint", 4);
        flag("focus.auto", true);
        flag("focus.manual", true);
        number("focus.pos", 0.62);
        flag("torch", false);
        flag("torch.available", true);
        flag("hdr", false);
        flag("hdr.available", true);
        flag("stabilization", true);
        flag("stabilization.available", true);
        flag("mirror", false);
    }

    // Übernimmt einen "set"-Befehl wie die App: Werte schalten die jeweilige Automatik aus.
    void Apply(const std::map<std::string, JsonValue>& command) {
        std::lock_guard lock(m_mutex);
        for (const auto& [key, value] : command) {
            if (key == "cmd" || key == "lenses" || key.ends_with(".min") || key.ends_with(".max") ||
                key.ends_with(".available") || key.ends_with(".manual")) {
                continue;
            }
            auto it = m_values.find(key);
            if (it == m_values.end() || it->second.type != value.type) continue;
            it->second = value;
            if (key == "iso" || key == "shutter") m_values["exposure.auto"].boolean = false;
            if (key == "wb.temp" || key == "wb.tint") m_values["wb.auto"].boolean = false;
            if (key == "focus.pos") m_values["focus.auto"].boolean = false;
        }
        m_dirty = true;
    }

    // Die Automatik „regelt nach“: kleine Schwankungen, damit die Anzeige lebt.
    void Tick(double seconds) {
        std::lock_guard lock(m_mutex);
        auto set = [&](const char* key, double value) {
            JsonValue& v = m_values[key];
            if (v.number != value) {
                v.number = value;
                m_dirty = true;
            }
        };
        if (m_values["exposure.auto"].boolean) {
            const double ev = m_values["ev"].number;
            set("iso", std::round(125.0 * std::pow(2.0, ev) * (1.0 + 0.08 * std::sin(seconds * 0.9))));
            set("shutter", 1.0 / 60.0);
        }
        if (m_values["wb.auto"].boolean) set("wb.temp", std::round((5200.0 + 120.0 * std::sin(seconds * 0.4)) / 10.0) * 10.0);
        if (m_values["focus.auto"].boolean) set("focus.pos", std::round((0.62 + 0.03 * std::sin(seconds * 1.3)) * 1000.0) / 1000.0);
    }

    // Flaches JSON, nur wenn sich seit dem letzten Mal etwas geändert hat.
    bool TakeState(std::string& json) {
        std::lock_guard lock(m_mutex);
        if (!m_dirty) return false;
        m_dirty = false;
        json = "{";
        for (const auto& [key, value] : m_values) {
            if (json.size() > 1) json += ",";
            json += "\"" + key + "\":";
            char number[32];
            switch (value.type) {
            case JsonValue::Type::Bool: json += value.boolean ? "true" : "false"; break;
            case JsonValue::Type::String: json += "\"" + value.text + "\""; break;
            default:
                snprintf(number, sizeof(number), "%.6g", value.number);
                json += number;
                break;
            }
        }
        json += "}";
        return true;
    }

    // Wirkung aufs Testbild (NV12, Videobereich): Helligkeit, Farbstich, Spiegeln.
    void ApplyLook(std::vector<uint8_t>& nv12, UINT width, UINT height) {
        double gain = 1.0;
        double warm = 0.0;
        double tint = 0.0;
        bool mirror = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_values["exposure.auto"].boolean) {
                gain = std::pow(2.0, m_values["ev"].number * 0.5);
            } else {
                const double exposure = m_values["iso"].number * m_values["shutter"].number;
                gain = std::sqrt(exposure / (125.0 / 60.0));
            }
            if (!m_values["wb.auto"].boolean) {
                // Niedrige Kelvin-Werte: die Kamera gleicht warmes Licht aus, das Bild wird kühler.
                warm = (m_values["wb.temp"].number - 5200.0) / 2800.0;
                tint = m_values["wb.tint"].number / 100.0;
            }
            mirror = m_values["mirror"].boolean;
        }
        gain = std::clamp(gain, 0.15, 4.0);
        if (std::fabs(gain - 1.0) > 0.01) {
            uint8_t table[256];
            for (int i = 0; i < 256; ++i) table[i] = static_cast<uint8_t>(std::clamp(16.0 + (i - 16) * gain, 16.0, 235.0));
            for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i) nv12[i] = table[nv12[i]];
        }
        if (warm != 0.0 || tint != 0.0) {
            const int du = static_cast<int>(-18.0 * warm + 6.0 * tint);  // Cb: blau
            const int dv = static_cast<int>(18.0 * warm + 8.0 * tint);   // Cr: rot
            uint8_t* uv = nv12.data() + static_cast<size_t>(width) * height;
            for (size_t i = 0, n = static_cast<size_t>(width) * height / 2; i < n; i += 2) {
                uv[i] = static_cast<uint8_t>(std::clamp(uv[i] + du, 16, 240));
                uv[i + 1] = static_cast<uint8_t>(std::clamp(uv[i + 1] + dv, 16, 240));
            }
        }
        if (mirror) {
            for (UINT y = 0; y < height; ++y) std::reverse(nv12.begin() + y * width, nv12.begin() + (y + 1) * width);
            uint16_t* uv = reinterpret_cast<uint16_t*>(nv12.data() + static_cast<size_t>(width) * height);
            for (UINT y = 0; y < height / 2; ++y) std::reverse(uv + y * (width / 2), uv + (y + 1) * (width / 2));
        }
    }

private:
    void Set(const char* key, const JsonValue& value) { m_values[key] = value; }

    std::mutex m_mutex;
    std::map<std::string, JsonValue> m_values;
    bool m_dirty = true;
};

void ServeClient(SOCKET client, const Options& options) {
    // HELLO wie von der iPhone-App, mit einem Ablaufdatum in 5 Tagen zum Testen der Anzeige.
    const long long expires = static_cast<long long>(time(nullptr)) + 5 * 86400;
    char hello[512];
    snprintf(hello, sizeof(hello),
             "{\"version\":1,\"app\":\"KrakznCam-Testquelle\",\"codec\":\"h264\",\"width\":%u,\"height\":%u,"
             "\"fps\":%u,\"bitrate\":%u,\"lens\":\"test\",\"signatureExpires\":%lld}",
             options.width, options.height, options.fps, options.bitrate, expires);
    std::vector<uint8_t> helloPayload(hello, hello + strlen(hello));
    if (!SendMessageTo(client, KrakznCam::kMsgHello, helloPayload)) return;

    // Befehle vom PC ("keyframe", "set") in einem eigenen Thread lesen.
    FakeCamera camera(options.fps);
    std::atomic<bool> forceKeyframe{true};
    std::atomic<bool> connected{true};
    std::thread reader([&] {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        while (KrakznCam::ReadMessage(client, type, payload)) {
            if (type != KrakznCam::kMsgCommand) continue;
            std::string text(payload.begin(), payload.end());
            std::map<std::string, JsonValue> command;
            if (!ParseJsonObject(text, command)) continue;
            const auto cmd = command.find("cmd");
            if (cmd == command.end()) continue;
            printf("Befehl erhalten: %s\n", text.c_str());
            fflush(stdout);
            if (cmd->second.text == "keyframe") forceKeyframe = true;
            else if (cmd->second.text == "set") camera.Apply(command);
        }
        connected = false;
    });
    // Zustand viermal pro Sekunde melden, aber nur bei Änderungen (wie die App).
    std::thread stateSender([&] {
        const auto start = std::chrono::steady_clock::now();
        std::string json;
        while (connected) {
            camera.Tick(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            if (camera.TakeState(json)) {
                std::vector<uint8_t> statePayload(json.begin(), json.end());
                if (!SendMessageTo(client, KrakznCam::kMsgState, statePayload)) break;
            }
            for (int i = 0; i < 5 && connected; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    Encoder encoder;
    TestScene scene;
    if (!scene.Init(options.width, options.height, options.fps)) {
        printf("Testbild konnte nicht vorbereitet werden.\n");
    } else if (encoder.Init(options)) {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const auto frameDuration = std::chrono::microseconds(1'000'000 / options.fps);
        auto windowStart = start;
        uint64_t windowFrames = 0;
        uint64_t windowBytes = 0;
        std::vector<EncodedFrame> encoded;
        std::vector<uint8_t> picture;
        for (int64_t index = 0; connected; ++index) {
            encoded.clear();
            picture = scene.Frame(index);
            camera.ApplyLook(picture, options.width, options.height);
            if (!encoder.Encode(picture, index, forceKeyframe.exchange(false), encoded)) break;
            bool sendFailed = false;
            for (const EncodedFrame& frame : encoded) {
                std::vector<uint8_t> payload;
                payload.reserve(9 + frame.data.size());
                KrakznCam::AppendBE64(payload, static_cast<uint64_t>(index * 1'000'000 / options.fps));
                payload.push_back(frame.keyframe ? 1 : 0);
                payload.insert(payload.end(), frame.data.begin(), frame.data.end());
                if (!SendMessageTo(client, KrakznCam::kMsgVideoFrame, payload)) {
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
    connected = false;
    shutdown(client, SD_BOTH);
    reader.join();
    stateSender.join();
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
