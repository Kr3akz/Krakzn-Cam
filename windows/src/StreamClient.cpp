#include "StreamClient.h"

#include "H264Decoder.h"
#include "Json.h"
#include "Log.h"
#include "StreamProtocol.h"
#include "Usbmux.h"

#include <chrono>
#include <map>
#include <vector>

StreamClient::StreamClient(ID3D11Device* device, FrameStore& store, HANDLE frameEvent)
    : m_device(device), m_store(store), m_frameEvent(frameEvent) {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

StreamClient::~StreamClient() {
    Stop();
    CloseHandle(m_stopEvent);
}

void StreamClient::Start(const std::string& directHost, uint16_t directPort) {
    if (m_running) return;
    m_directHost = directHost;
    m_directPort = directPort;
    m_status.direct = !directHost.empty();
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { Run(); });
}

void StreamClient::Stop() {
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    {
        std::lock_guard lock(m_mutex);
        if (m_socket != INVALID_SOCKET) shutdown(m_socket, SD_BOTH);  // weckt ein blockierendes recv()
    }
    if (m_thread.joinable()) m_thread.join();
}

StreamStatus StreamClient::Status() {
    std::lock_guard lock(m_mutex);
    return m_status;
}

void StreamClient::SetState(StreamStatus::State state, const std::string& detail) {
    std::lock_guard lock(m_mutex);
    if (m_status.state != state || m_status.detail != detail) {
        LogLine("Status: %d %s", static_cast<int>(state), detail.c_str());
    }
    m_status.state = state;
    m_status.detail = detail;
    if (state != StreamStatus::State::Streaming && state != StreamStatus::State::Connected) {
        m_status.measuredFps = 0;
        m_status.measuredMbps = 0;
        m_status.width = m_status.height = m_status.fps = 0;
        m_status.bitrate = 0;
        m_status.lens.clear();
        m_status.signatureExpires = 0;
    }
}

bool StreamClient::WaitForStop(DWORD milliseconds) {
    return WaitForSingleObject(m_stopEvent, milliseconds) == WAIT_OBJECT_0;
}

void StreamClient::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (m_running) {
        SOCKET s = OpenConnection();
        if (s == INVALID_SOCKET) {
            if (WaitForStop(1000)) break;
            continue;
        }
        {
            std::lock_guard lock(m_mutex);
            m_socket = s;
        }
        Serve(s);
        {
            std::lock_guard lock(m_mutex);
            m_socket = INVALID_SOCKET;
        }
        closesocket(s);
        if (!m_running) break;
        LogLine("Verbindung beendet, versuche es erneut");
        SetState(StreamStatus::State::Starting);
        if (WaitForStop(500)) break;
    }
    CoUninitialize();
}

SOCKET StreamClient::OpenConnection() {
    if (!m_directHost.empty()) {
        SOCKET s = TcpConnect(m_directHost.c_str(), m_directPort, 1000);
        if (s == INVALID_SOCKET) SetState(StreamStatus::State::AppNotReachable, "Testquelle nicht erreichbar");
        return s;
    }

    std::vector<UsbmuxDevice> devices;
    if (!Usbmux::ListDevices(devices)) {
        SetState(StreamStatus::State::NoAppleService);
        return INVALID_SOCKET;
    }
    const UsbmuxDevice* usb = nullptr;
    for (const UsbmuxDevice& device : devices) {
        if (device.connectionType == "USB") {
            usb = &device;
            break;
        }
    }
    if (!usb) {
        SetState(StreamStatus::State::NoDevice);
        return INVALID_SOCKET;
    }
    {
        std::lock_guard lock(m_mutex);
        m_status.deviceSerial = usb->serial;
    }
    int result = -1;
    SOCKET s = Usbmux::Connect(usb->id, KabelCam::kPort, result);
    if (s == INVALID_SOCKET) {
        SetState(StreamStatus::State::AppNotReachable,
                 result == 3 || result == -1 ? std::string() : "usbmux-Fehler " + std::to_string(result));
    }
    return s;
}

void StreamClient::HandleHello(const std::vector<uint8_t>& payload) {
    std::map<std::string, JsonValue> hello;
    std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (!ParseJsonObject(json, hello)) {
        LogLine("HELLO konnte nicht gelesen werden");
        return;
    }
    auto number = [&](const char* key) -> double {
        auto it = hello.find(key);
        return it != hello.end() && it->second.type == JsonValue::Type::Number ? it->second.number : 0.0;
    };
    auto text = [&](const char* key) -> std::string {
        auto it = hello.find(key);
        return it != hello.end() && it->second.type == JsonValue::Type::String ? it->second.text : std::string();
    };
    std::lock_guard lock(m_mutex);
    m_status.width = static_cast<int>(number("width"));
    m_status.height = static_cast<int>(number("height"));
    m_status.fps = static_cast<int>(number("fps"));
    m_status.bitrate = static_cast<int64_t>(number("bitrate"));
    m_status.lens = text("lens");
    m_status.signatureExpires = static_cast<int64_t>(number("signatureExpires"));
    LogLine("HELLO: %dx%d @ %d FPS, %lld bit/s, Objektiv %s, Signatur bis %lld", m_status.width, m_status.height,
            m_status.fps, static_cast<long long>(m_status.bitrate), m_status.lens.c_str(),
            static_cast<long long>(m_status.signatureExpires));
}

void StreamClient::Serve(SOCKET s) {
    using Clock = std::chrono::steady_clock;
    SetRecvTimeout(s, 5000);
    SetState(StreamStatus::State::Connected);

    H264Decoder decoder;
    if (!decoder.Initialize(m_device.Get())) {
        SetState(StreamStatus::State::Connected, "Decoder konnte nicht gestartet werden");
        WaitForStop(2000);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_status.hardwareDecoding = decoder.IsHardware();
    }
    KabelCam::SendCommand(s, "{\"cmd\":\"keyframe\"}");

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    auto windowStart = Clock::now();
    auto lastKeyframeRequest = Clock::now() - std::chrono::seconds(10);
    uint64_t windowFrames = 0;
    uint64_t windowBytes = 0;

    while (m_running && KabelCam::ReadMessage(s, type, payload)) {
        if (type == KabelCam::kMsgHello) {
            HandleHello(payload);
        } else if (type == KabelCam::kMsgVideoFrame && payload.size() > 9) {
            int64_t pts = static_cast<int64_t>(KabelCam::ReadBE64(payload.data()));
            const uint8_t* data = payload.data() + 9;
            size_t size = payload.size() - 9;
            windowBytes += size;
            bool ok = decoder.Decode(data, size, pts, [&](const DecodedFrame& frame) {
                if (frame.texture) {
                    m_store.StoreFromTexture(frame.texture, frame.subresource, frame.displayWidth,
                                             frame.displayHeight, frame.ptsMicros);
                } else if (frame.data) {
                    m_store.StoreFromMemory(frame.data, frame.stride, frame.codedWidth, frame.codedHeight,
                                            frame.displayWidth, frame.displayHeight, frame.ptsMicros);
                }
                ++windowFrames;
                {
                    std::lock_guard lock(m_mutex);
                    ++m_status.framesDecoded;
                    m_status.state = StreamStatus::State::Streaming;
                    m_status.detail.clear();
                }
                SetEvent(m_frameEvent);
            });
            // Nach einem Fehler höchstens einmal pro Sekunde einen neuen Keyframe anfordern.
            if (!ok && Clock::now() - lastKeyframeRequest > std::chrono::seconds(1)) {
                lastKeyframeRequest = Clock::now();
                KabelCam::SendCommand(s, "{\"cmd\":\"keyframe\"}");
            }
        }

        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - windowStart).count();
        if (elapsed >= 1.0) {
            std::lock_guard lock(m_mutex);
            m_status.measuredFps = static_cast<double>(windowFrames) / elapsed;
            m_status.measuredMbps = static_cast<double>(windowBytes) * 8.0 / elapsed / 1e6;
            windowStart = now;
            windowFrames = 0;
            windowBytes = 0;
        }
    }
}
