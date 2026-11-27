#pragma once
#include "FrameStore.h"
#include "VCamShared.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct IMFVirtualCamera;

// Die virtuelle Kamera „KrakznCam“ (Windows 11), z. B. für OBS.
// - Meldet die Kamera bei Windows an, solange KrakznCam läuft (danach verschwindet sie wieder).
// - Rechnet Bilder nur um, solange ein Programm die Kamera wirklich benutzt.
// - Die Kamera-DLL muss einmalig mit Administratorrechten eingerichtet werden (InstallAsync).
class VirtualCamera {
public:
    enum class State {
        Unsupported,   // älter als Windows 11
        NotInstalled,  // Kamera-DLL noch nicht eingerichtet
        Starting,
        Ready,         // angemeldet, kein Programm schaut zu
        InUse,         // ein Programm benutzt die Kamera
        Failed,
    };
    struct Status {
        State state = State::Starting;
        std::string detail;
        int width = 0;  // bei InUse: gewünschtes Format
        int height = 0;
        int fps = 0;
        bool updateAvailable = false;  // DLL neben krakzncam.exe ist neuer als die eingerichtete
        bool installing = false;
        std::string installError;      // letzte Einrichtung fehlgeschlagen
    };

    VirtualCamera(ID3D11Device* device, FrameStore& store);
    ~VirtualCamera();

    // registerCamera = false: nur Bilder liefern (Test mit KRAKZNCAM_VCAM_SHARED, siehe VCamShared.h).
    void Start(bool registerCamera = true);
    void Stop();
    // Wird bei jedem neuen Bild gesetzt (StreamClient::SetExtraFrameEvent).
    HANDLE FrameEvent() const { return m_frameEvent; }
    Status GetStatus();

    // Einrichtung bzw. Aktualisierung: startet krakzncam.exe --install-camera mit UAC-Abfrage.
    void InstallAsync();

    // Im Prozess mit Administratorrechten (--install-camera / --uninstall-camera). 0 = Erfolg.
    static int InstallElevated();
    static int UninstallElevated();

private:
    void Run();
    void RefreshInstallState();
    bool Register();
    void Unregister();
    bool OpenShared();
    void CloseShared();
    void Feed();
    bool ConfigureProcessor(ID3D11Texture2D* input, const FrameInfo& info, UINT width, UINT height);
    void SetState(State state, const std::string& detail = {});

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    FrameStore& m_store;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_wakeEvent = nullptr;
    std::thread m_thread;
    std::thread m_installThread;
    bool m_register = true;
    std::atomic<bool> m_retryRegistration{false};

    std::mutex m_mutex;
    Status m_status;
    bool m_installed = false;

    // Nur im Arbeits-Thread:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> m_camera;
    HANDLE m_mapping = nullptr;
    VCam::Header* m_header = nullptr;
    ULONGLONG m_lastOpenAttempt = 0;
    ULONGLONG m_lastWrite = 0;
    uint64_t m_lastCounter = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    ID3D11Texture2D* m_configuredInput = nullptr;
    FrameInfo m_configuredInfo;
    UINT m_outputWidth = 0;
    UINT m_outputHeight = 0;
    ULONGLONG m_processorRetryAt = 0;  // nach einem Fehler später erneut versuchen
};
