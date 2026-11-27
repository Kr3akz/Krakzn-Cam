#pragma once
#include "CameraFeeder.h"
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
// - Seit der Einrichtung ist sie dauerhaft bei Windows angemeldet, OBS findet sie also auch,
//   wenn KrakznCam geschlossen ist (die Kamera-DLL empfängt das iPhone dann selbst).
// - Läuft KrakznCam, liefert es die Bilder und die Kamera-DLL hält sich zurück.
// - Ältere Einrichtungen ohne dauerhafte Anmeldung: Anmeldung nur, solange KrakznCam läuft.
// - Rechnet Bilder nur um, solange ein Programm die Kamera wirklich benutzt.
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
        bool persistent = false;       // dauerhaft angemeldet (auch ohne laufendes KrakznCam)
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
    void SetState(State state, const std::string& detail = {});

    FrameStore& m_store;
    CameraFeeder m_feeder;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_appMutex = nullptr;  // zeigt der Kamera-DLL: KrakznCam läuft und liefert die Bilder
    std::thread m_thread;
    std::thread m_installThread;
    bool m_register = true;
    std::atomic<bool> m_retryRegistration{false};

    std::mutex m_mutex;
    Status m_status;
    bool m_installed = false;
    bool m_persistent = false;

    // Nur im Arbeits-Thread:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> m_camera;
};
