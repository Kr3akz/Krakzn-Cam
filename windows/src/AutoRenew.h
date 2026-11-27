#pragma once
#include "SetupCheck.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>

// Erneuert die App-Signatur nur bei Bedarf: KrakznCam startet den Sideloadly-Hintergrunddienst,
// wenn die Signatur in weniger als 24 Stunden abläuft, und beendet ihn wieder, sobald sie
// erneuert ist (oder KrakznCam geschlossen wird). Sideloadlys eigener Windows-Autostart wird
// dafür abgeschaltet und beim Ausschalten der Funktion wiederhergestellt.
//
// Warum 24 Stunden: Sideloadly erneuert frühestens nach 96 Stunden (4 von 7 Tagen); am letzten
// Tag ist die Erneuerung also sicher fällig und passiert sofort. Später heißt seltener erneuern.
// Ist die Signatur schon abgelaufen, startet der Dienst beim nächsten Start von KrakznCam und
// holt die Erneuerung nach (das gespeicherte Ablaufdatum reicht dafür).
class AutoRenew {
public:
    enum class State {
        Off,           // Funktion ausgeschaltet
        NoSideloadly,  // Sideloadly nicht gefunden
        Waiting,       // Signatur noch lange gültig (oder Ablauf unbekannt)
        Renewing,      // läuft bald ab, Sideloadly-Dienst läuft und erneuert
    };

    // active = false: nur Zustand anzeigen, nichts speichern, keinen Dienst starten, nichts an
    // Windows ändern (für Selbsttests).
    explicit AutoRenew(bool active = true);
    ~AutoRenew();

    // Regelmäßig aufrufen (prüft höchstens alle 2 s). signatureExpires: Ablauf aus dem HELLO
    // der iPhone-App in Unix-Sekunden, 0 wenn gerade unbekannt. Der letzte Wert wird gespeichert.
    void Update(int64_t signatureExpires, const SetupReport& setup);

    void SetEnabled(bool enabled);
    bool Enabled() const { return m_enabled; }
    State GetState() const { return m_state; }
    int64_t KnownExpiry() const { return m_expiry; }

    static constexpr int64_t kRenewBefore = 24 * 3600;

private:
    void Save();
    void RemoveSideloadlyAutostart();
    void RestoreSideloadlyAutostart();
    void StartDaemon(const std::wstring& exe);
    void StopDaemon();

    std::wstring m_iniPath;
    bool m_active = true;
    bool m_enabled = true;
    int64_t m_expiry = 0;
    State m_state = State::Waiting;
    HANDLE m_daemon = nullptr;  // nur gesetzt, wenn KrakznCam den Dienst selbst gestartet hat
    bool m_autostartChecked = false;
    std::chrono::steady_clock::time_point m_lastCheck{};
};
