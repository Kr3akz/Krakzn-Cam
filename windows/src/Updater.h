#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Sucht nach neuen KrakznCam-Versionen (GitHub-Releases), lädt das Setup herunter und startet es.
//
// - Prüft kurz nach dem Start und danach alle 6 Stunden, solange KrakznCam läuft.
// - Ein Release gilt als Update, wenn sein Tag (z. B. „v0.2.0“) neuer ist als die eigene Version
//   und es die Datei „KrakznCam-Setup.exe“ enthält.
// - Der Download wird an Größe und, falls GitHub sie angibt, SHA-256-Prüfsumme geprüft.
// - Das Setup wartet, bis KrakznCam beendet ist, ersetzt die Dateien und startet KrakznCam neu.
class Updater {
public:
    enum class State {
        Off,          // abgeschaltet (Selbsttest)
        Idle,         // noch nicht geprüft
        Checking,
        UpToDate,
        Available,    // neue Version gefunden
        Downloading,
        Ready,        // heruntergeladen und geprüft
        Failed,
    };
    struct Status {
        State state = State::Idle;
        std::string version;  // gefundene Version, z. B. „0.2.0“
        float progress = 0.0f;
        std::string error;
    };

    static constexpr const wchar_t* kDefaultFeed = L"https://api.github.com/repos/Kr3akz/Krakzn-Cam/releases/latest";
    static constexpr const char* kSetupAssetName = "KrakznCam-Setup.exe";

    // feed: Adresse der Release-Beschreibung (zum Testen änderbar). active = false: nie prüfen.
    Updater(std::string currentVersion, std::wstring feed, bool active);
    ~Updater();

    Status GetStatus();
    void CheckNow();
    void Download();
    // Startet das heruntergeladene Setup. true = KrakznCam soll sich jetzt beenden.
    bool LaunchInstaller();

private:
    void Run();
    void Check();
    void DoDownload();
    void SetState(State state, const std::string& error = {});

    const std::string m_current;
    const std::wstring m_feed;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_checkRequested{false};
    std::atomic<bool> m_downloadRequested{false};
    std::atomic<bool> m_retrySoon{false};

    std::mutex m_mutex;
    Status m_status;
    std::wstring m_assetUrl;
    uint64_t m_assetSize = 0;
    std::string m_assetDigest;  // „sha256:…“ oder leer
    std::wstring m_downloadedFile;
};

// Vergleicht Versionen wie „0.2.0“ oder „v1.10.3“: <0, 0, >0. Unbekanntes zählt als 0.
int CompareVersions(const std::string& a, const std::string& b);
