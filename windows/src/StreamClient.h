#pragma once
#include "FrameStore.h"
#include "Net.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StreamStatus {
    enum class State {
        Starting,
        NoAppleService,   // Apple Mobile Device Service nicht erreichbar
        NoDevice,         // kein iPhone per USB
        AppNotReachable,  // iPhone da, aber KrakznCam-App lauscht nicht
        Connected,        // verbunden, noch kein Bild
        Streaming,        // Bilder kommen an
    };
    State state = State::Starting;
    std::string detail;
    std::string deviceSerial;
    bool direct = false;  // Testquelle statt iPhone

    // aus HELLO
    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t bitrate = 0;
    std::string lens;
    int64_t signatureExpires = 0;  // Unix-Sekunden, 0 = unbekannt

    // gemessen
    double measuredFps = 0.0;
    double measuredMbps = 0.0;
    uint64_t framesDecoded = 0;
    bool hardwareDecoding = false;
};

// Empfangs-Thread: sucht das iPhone, baut den USB-Tunnel auf, empfängt und dekodiert.
// Bei Verbindungsverlust versucht er es von selbst erneut.
class StreamClient {
public:
    StreamClient(ID3D11Device* device, FrameStore& store, HANDLE frameEvent);
    ~StreamClient();

    // Leerer Host = iPhone über USB. Sonst direkte TCP-Verbindung (Testquelle).
    void Start(const std::string& directHost, uint16_t directPort);
    void Stop();
    StreamStatus Status();

private:
    void Run();
    SOCKET OpenConnection();
    void Serve(SOCKET s);
    void HandleHello(const std::vector<uint8_t>& payload);
    void SetState(StreamStatus::State state, const std::string& detail = {});
    bool WaitForStop(DWORD milliseconds);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    FrameStore& m_store;
    HANDLE m_frameEvent;
    HANDLE m_stopEvent;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::string m_directHost;
    uint16_t m_directPort = 0;

    std::mutex m_mutex;
    StreamStatus m_status;
    SOCKET m_socket = INVALID_SOCKET;
};
