// KrakznCam für Windows: empfängt das iPhone-Bild per USB und zeigt es an.
//
// Aufruf:
//   krakzncam.exe                               normaler Start
//   krakzncam.exe --direct 127.0.0.1:17878      Testquelle statt iPhone (krakzncam_fakephone.exe)
//   krakzncam.exe --check --log datei.txt       nur Einrichtung prüfen und ins Protokoll schreiben
//   krakzncam.exe --install-camera              virtuelle Kamera einrichten (braucht Administratorrechte)
//   krakzncam.exe --uninstall-camera            virtuelle Kamera wieder entfernen
//   krakzncam.exe --cleanup                     vor dem Deinstallieren: Sideloadly-Autostart zurückgeben
//   krakzncam.exe --update-feed URL             Updates von einer anderen Adresse suchen (Test)
//   krakzncam.exe --selftest --frames 60 --snapshot bild.png --screenshot ui.png --log datei.txt
//                                              unsichtbar laufen, bis N Bilder dekodiert sind, dann speichern
//                [--click x,y] [--wheel x,y,menge]   danach Mausklicks bzw. Mausrad simulieren (mehrfach möglich)

#include "App.h"
#include "AutoRenew.h"
#include "FrameStore.h"
#include "Log.h"
#include "Net.h"
#include "SetupCheck.h"
#include "Snapshot.h"
#include "StreamClient.h"
#include "TextUtil.h"
#include "UiHost.h"
#include "Updater.h"
#include "VirtualCamera.h"

#include "imgui.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string directHost;
    uint16_t directPort = 0;
    std::wstring logPath;
    bool check = false;
    bool installCamera = false;    // --install-camera: virtuelle Kamera einrichten (Administrator)
    bool uninstallCamera = false;  // --uninstall-camera: wieder entfernen (Administrator)
    bool cleanup = false;          // --cleanup: Änderungen an Sideloadly zurücknehmen (Deinstallation)
    bool vcam = false;             // Selbsttest: virtuelle Kamera anmelden
    bool vcamFeed = false;         // Selbsttest: virtuelle Kamera nur mit Bildern versorgen
    std::wstring updateFeed;       // leer = GitHub-Releases
    bool selftest = false;
    int frames = 60;        // Selbsttest: so viele Bilder abwarten (0 = nur Zeit ablaufen lassen)
    double timeout = 30.0;  // Selbsttest: höchstens so viele Sekunden
    int windowWidth = 1280;  // Selbsttest: Fenstergröße
    int windowHeight = 760;
    App::Page page = App::Page::Camera;
    std::wstring snapshotPath;
    std::wstring screenshotPath;
    // Selbsttest: simulierte Eingaben nach dem letzten Bild (Fensterkoordinaten).
    struct InputAction {
        bool wheel = false;
        float x = 0.0f;
        float y = 0.0f;
        float amount = 0.0f;
    };
    std::vector<InputAction> actions;
};

Options ParseOptions() {
    Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--direct") {
            std::string value = Utf8(next());
            size_t colon = value.rfind(':');
            if (colon != std::string::npos) {
                options.directHost = value.substr(0, colon);
                options.directPort = static_cast<uint16_t>(atoi(value.c_str() + colon + 1));
            }
        } else if (arg == L"--log") {
            options.logPath = next();
        } else if (arg == L"--check") {
            options.check = true;
        } else if (arg == L"--install-camera") {
            options.installCamera = true;
        } else if (arg == L"--uninstall-camera") {
            options.uninstallCamera = true;
        } else if (arg == L"--cleanup") {
            options.cleanup = true;
        } else if (arg == L"--vcam") {
            options.vcam = true;
        } else if (arg == L"--vcam-feed") {
            options.vcamFeed = true;
        } else if (arg == L"--update-feed") {
            options.updateFeed = next();
        } else if (arg == L"--selftest") {
            options.selftest = true;
        } else if (arg == L"--frames") {
            options.frames = _wtoi(next().c_str());
        } else if (arg == L"--timeout") {
            options.timeout = _wtof(next().c_str());
        } else if (arg == L"--page") {
            options.page = next() == L"setup" ? App::Page::Setup : App::Page::Camera;
        } else if (arg == L"--size") {
            const std::wstring value = next();  // z. B. 1920x1080
            const size_t x = value.find(L'x');
            if (x != std::wstring::npos) {
                options.windowWidth = _wtoi(value.substr(0, x).c_str());
                options.windowHeight = _wtoi(value.substr(x + 1).c_str());
            }
        } else if (arg == L"--snapshot") {
            options.snapshotPath = next();
        } else if (arg == L"--screenshot") {
            options.screenshotPath = next();
        } else if (arg == L"--click" || arg == L"--wheel") {
            Options::InputAction action;
            action.wheel = arg == L"--wheel";
            swscanf_s(next().c_str(), L"%f,%f,%f", &action.x, &action.y, &action.amount);
            options.actions.push_back(action);
        }
    }
    LocalFree(argv);
    return options;
}

void Frame(UiHost& host, App& app, bool present) {
    app.UpdateVideo();
    host.BeginFrame();
    app.Draw();
    host.EndFrame(present);
}

// Normale Schleife. Gezeichnet wird nur bei neuen Bildern, Eingaben oder alle 250 ms für den
// Status. Ist das Fenster minimiert, wird gar nicht gezeichnet.
int RunLoop(UiHost& host, App& app, HANDLE frameEvent) {
    int extraFrames = 2;
    for (;;) {
        const DWORD timeout = extraFrames > 0 || app.Animating() ? 0 : 250;
        MsgWaitForMultipleObjectsEx(1, &frameEvent, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        bool hadInput = false;
        if (!host.ProcessMessages(&hadInput)) break;
        if (hadInput) extraFrames = 2;  // ImGui braucht nach Eingaben noch ein, zwei Bilder
        // Minimiert oder ganz verdeckt: nichts zeichnen. Empfang und virtuelle Kamera laufen weiter.
        if (host.Minimized() || host.Occluded()) {
            extraFrames = 0;
            continue;
        }
        // Nimmt der Bildschirm gerade kein Bild an (Fenster im Hintergrund), dieses Bild auslassen,
        // statt in Present() zu warten und dabei Decoder und virtuelle Kamera aufzuhalten.
        if (!host.ReadyForFrame(20)) continue;
        Frame(host, app, true);
        if (extraFrames > 0) --extraFrames;
    }
    return 0;
}

int RunSelftest(UiHost& host, App& app, StreamClient& client, HANDLE frameEvent, const Options& options) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(options.timeout * 1000.0));
    bool success = options.frames <= 0;  // ohne Bildziel: nur die Zeit ablaufen lassen
    while (Clock::now() < deadline) {
        WaitForSingleObject(frameEvent, 100);
        host.ProcessMessages();
        host.ReadyForFrame(100);
        Frame(host, app, true);
        if (options.frames > 0 && static_cast<int>(client.Status().framesDecoded) >= options.frames &&
            app.Video().HasFrame()) {
            success = true;
            break;
        }
    }
    auto pump = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            Sleep(16);
            host.ProcessMessages();
            host.ReadyForFrame(100);
            Frame(host, app, true);
        }
    };
    // Simulierte Klicks und Mausrad, jeweils mit Zeit für die Antwort des iPhones.
    for (const Options::InputAction& action : options.actions) {
        const ImVec2 pos(action.x, action.y);
        host.pendingInput.push_back([pos](ImGuiIO& io) { io.AddMousePosEvent(pos.x, pos.y); });
        if (action.wheel) {
            const float amount = action.amount;
            host.pendingInput.push_back([amount](ImGuiIO& io) { io.AddMouseWheelEvent(0.0f, amount); });
        } else {
            host.pendingInput.push_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
            host.pendingInput.push_back([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
        }
        pump(4);
        LogLine("Selbsttest: %s bei %.0f,%.0f", action.wheel ? "Mausrad" : "Klick", action.x, action.y);
        pump(36);
    }
    if (!options.actions.empty()) {
        host.pendingInput.push_back([](ImGuiIO& io) { io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); });
    }
    pump(40);  // Animationen auslaufen lassen (Seite einblenden, Hintergrund)
    Frame(host, app, false);
    if (!options.screenshotPath.empty()) {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(host.SwapChain()->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            SaveTexturePng(host.Device(), host.Context(), backBuffer.Get(), options.screenshotPath);
        }
    }
    if (!options.snapshotPath.empty() && app.Video().HasFrame()) {
        SaveTexturePng(host.Device(), host.Context(), app.Video().OutputTexture(), options.snapshotPath);
    }

    const StreamStatus status = client.Status();
    LogLine("SELBSTTEST %s: %llu Bilder dekodiert, %.1f FPS, %.2f Mbit/s, Decoder: %s, Vorschau %ux%u, HELLO %dx%d",
            success ? "OK" : "FEHLGESCHLAGEN", static_cast<unsigned long long>(status.framesDecoded),
            status.measuredFps, status.measuredMbps, status.hardwareDecoding ? "Grafikkarte" : "Software",
            app.Video().Width(), app.Video().Height(), status.width, status.height);
    return success ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const Options options = ParseOptions();
    std::wstring logPath = options.logPath.empty() ? DefaultLogPath() : options.logPath;
    if (options.logPath.empty() && (options.installCamera || options.uninstallCamera || options.cleanup)) {
        // Eigene Datei: Ein laufendes KrakznCam hält sein Protokoll offen.
        const size_t slash = logPath.find_last_of(L'\\');
        logPath = logPath.substr(0, slash + 1) + L"einrichtung.log";
    }
    LogInit(logPath);
    LogLine("KrakznCam %s startet", KRAKZNCAM_VERSION);

    // Windows 11 drosselt Programme, deren Fenster nicht vorne ist („Effizienzmodus“: langsamere
    // Kerne, gröbere Zeitgeber). KrakznCam muss im Hintergrund genauso schnell empfangen und an
    // OBS liefern, deshalb davon ausnehmen.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    NetInit();

    // Einmalige Einrichtung der virtuellen Kamera (läuft mit Administratorrechten, ohne Fenster).
    if (options.installCamera || options.uninstallCamera) {
        const int result = options.installCamera ? VirtualCamera::InstallElevated() : VirtualCamera::UninstallElevated();
        MFShutdown();
        CoUninitialize();
        return result;
    }
    // Deinstallation: Sideloadlys Autostart wiederherstellen, falls KrakznCam ihn übernommen hatte.
    if (options.cleanup) {
        AutoRenew renew(true);
        if (renew.Enabled()) renew.SetEnabled(false);
        LogLine("Aufgeräumt für die Deinstallation");
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    if (options.check) {
        const SetupReport report = RunSetupCheck();
        for (const SetupItem& item : report.items) {
            LogLine("[%s] %s: %s%s%s", SetupStatusName(item.status), item.title.c_str(), item.detail.c_str(),
                    item.actionLabel.empty() ? "" : "  -> ", Utf8(item.actionTarget).c_str());
        }
        LogLine("Bereit für Sideloadly: %s", report.sideloadReady ? "ja" : "nein");
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    int exitCode = 0;
    {
        UiHost host;
        UiHost::Options window;
        window.instance = instance;
        window.videoSupport = true;
        if (options.selftest) {
            window.exactPixelSize = true;
            window.width = options.windowWidth;
            window.height = options.windowHeight;
        }
        if (!host.Create(window)) {
            MFShutdown();
            CoUninitialize();
            return 1;
        }

        FrameStore store(host.Device());
        HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // Die Kamera vor dem Empfang anlegen und nach ihm abbauen: Der Empfang meldet ihr neue Bilder.
        VirtualCamera camera(host.Device(), store);
        StreamClient client(host.Device(), store, frameEvent);
        client.SetExtraFrameEvent(camera.FrameEvent());
        // Im Selbsttest wird die Kamera nur auf Wunsch angemeldet (--vcam) oder nur beliefert (--vcam-feed).
        if (!options.selftest || options.vcam) camera.Start(true);
        else if (options.vcamFeed) camera.Start(false);
        client.Start(options.directHost, options.directPort);
        // Im Selbsttest nur mit ausdrücklicher Test-Adresse nach Updates suchen.
        Updater updater(KRAKZNCAM_VERSION, options.updateFeed.empty() ? Updater::kDefaultFeed : options.updateFeed,
                        !options.selftest || !options.updateFeed.empty());
        App app(host, client, store, camera, updater, options.page, host.DpiScale(), options.selftest);
        host.onActivated = [&app] { app.OnActivated(); };

        if (options.selftest) {
            exitCode = RunSelftest(host, app, client, frameEvent, options);
        } else {
            host.Show(showCommand);
            exitCode = RunLoop(host, app, frameEvent);
        }

        host.onActivated = nullptr;
        client.Stop();
        camera.Stop();
        CloseHandle(frameEvent);
    }

    MFShutdown();
    CoUninitialize();
    LogLine("KrakznCam beendet (Code %d)", exitCode);
    return exitCode;
}
