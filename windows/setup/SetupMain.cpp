// KrakznCam-Setup: Installieren, Aktualisieren und Entfernen, im Design von KrakznCam.
//
//   KrakznCam-Setup.exe                          Installation mit Oberfläche
//   KrakznCam-Setup.exe --update --wait-pid N    Update aus KrakznCam heraus (wartet auf dessen Ende,
//                                                installiert und startet KrakznCam neu)
//   uninstall.exe --uninstall                    Deinstallation (aus „Apps“ in den Einstellungen)
// Zum Testen:
//   --test-root ORDNER   alles in diesen Ordner statt ins System (ohne Kamera und Sideloadly-Aufräumen)
//   --auto               ohne Klicks durchlaufen
//   --preview SEITE --screenshot datei.png   Seite (welcome, update, progress, done, failed, uninstall,
//                                            removed) zeichnen und als Bild speichern

#include "Installer.h"
#include "Log.h"
#include "Snapshot.h"
#include "TextUtil.h"
#include "Theme.h"
#include "UiHost.h"
#include "Updater.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace Theme;
using Microsoft::WRL::ComPtr;

namespace {

enum class Mode { Install, Update, Uninstall };
enum class Page { Welcome, Working, Done, Failed, UninstallConfirm, Removed };

struct Options {
    Mode mode = Mode::Install;
    DWORD waitPid = 0;
    bool fromTemp = false;
    bool automatic = false;
    std::wstring testRoot;
    std::wstring preview;
    std::wstring screenshot;
};

Options ParseOptions() {
    Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (arg == L"--update") options.mode = Mode::Update;
        else if (arg == L"--uninstall") options.mode = Mode::Uninstall;
        else if (arg == L"--wait-pid") options.waitPid = static_cast<DWORD>(_wtoi(next().c_str()));
        else if (arg == L"--from-temp") options.fromTemp = true;
        else if (arg == L"--auto") options.automatic = true;
        else if (arg == L"--test-root") options.testRoot = next();
        else if (arg == L"--preview") options.preview = next();
        else if (arg == L"--screenshot") options.screenshot = next();
    }
    LocalFree(argv);
    return options;
}

// --- kleine Bausteine im Stil der App ------------------------------------------------------

float Kicker(ImDrawList* dl, ImVec2 pos, std::string_view text) {
    Spaced(dl, GetFonts().brand, Px(11.5f), pos, kText3, text, 0.2f);
    return Px(11.5f) * 1.25f;
}

float Note(ImDrawList* dl, ImVec2 pos, float width, std::string_view text, bool warn) {
    const ImVec2 padding(Px(14.0f), Px(11.0f));
    const float size = Px(14.5f);
    const float iconSize = Px(15.0f);
    const float textX = pos.x + padding.x + iconSize + Px(10.0f);
    const float textWidth = pos.x + width - padding.x - textX;
    const float height = Measure(GetFonts().regular, size, text, textWidth).y + padding.y * 2.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), A(warn ? WithAlpha(kGold, 0.10f) : White(0.07f)),
                      Px(9.0f));
    DrawIcon(dl, warn ? Icon::Warning : Icon::Info, ImVec2(pos.x + padding.x + iconSize * 0.5f, pos.y + padding.y + Px(9.5f)),
             iconSize, warn ? kGold : kText2);
    Text(dl, GetFonts().regular, size, ImVec2(textX, pos.y + padding.y), warn ? kGold : kText2, text, textWidth);
    return height;
}

// Zeile mit Titel, grauer Erklärung und Schalter rechts, auf einer hellen Kachel.
float OptionRow(ImDrawList* dl, const char* id, ImVec2 pos, float width, std::string_view title, std::string_view detail,
                bool& value) {
    const Fonts& fonts = GetFonts();
    const ImVec2 padding(Px(16.0f), Px(12.0f));
    const float textWidth = width - padding.x * 3.0f - SwitchWidth();
    const float titleHeight = Measure(fonts.regular, Px(15.0f), title).y;
    const float detailHeight = detail.empty() ? 0.0f : Px(2.0f) + Measure(fonts.regular, Px(13.0f), detail, textWidth).y;
    const float height = titleHeight + detailHeight + padding.y * 2.0f;
    dl->AddRectFilled(pos, pos + ImVec2(width, height), A(White(0.045f)), Px(12.0f));
    Text(dl, fonts.regular, Px(15.0f), pos + padding, kText, title);
    if (!detail.empty()) {
        Text(dl, fonts.regular, Px(13.0f), ImVec2(pos.x + padding.x, pos.y + padding.y + titleHeight + Px(2.0f)), kText3,
             detail, textWidth);
    }
    Switch(id, ImVec2(pos.x + width - padding.x, pos.y + height * 0.5f), value);
    return height;
}

// --- Setup-Fenster ---------------------------------------------------------------------------

class SetupApp {
public:
    SetupApp(UiHost& host, const Options& options) : m_host(host), m_options(options), m_paths(MakePaths(options.testRoot)) {
        m_installed = InstalledVersion(m_paths);
        m_cameraSupported = CameraSupported();
        m_camera = m_cameraSupported;
        switch (options.mode) {
        case Mode::Install: m_page = Page::Welcome; break;
        case Mode::Update: m_page = Page::Working; break;
        case Mode::Uninstall: m_page = Page::UninstallConfirm; break;
        }
        m_host.canClose = [this] { return !m_working; };
        if (!options.preview.empty()) {
            ApplyPreview();
            return;
        }
        if (options.mode == Mode::Update || (options.automatic && options.mode != Mode::Uninstall)) StartInstall();
        if (options.automatic && options.mode == Mode::Uninstall) StartUninstall();
    }

    ~SetupApp() {
        if (m_worker.joinable()) m_worker.join();
    }

    bool Animating() const { return m_animating || m_working; }
    bool WantsClose() const { return m_close; }

    void Draw() {
        BeginFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 size = viewport->Size;
        SetScale(size.x / 820.0f);
        PollWorker();

        ImDrawList* background = ImGui::GetBackgroundDrawList();
        Backdrop(background, ImVec2(0, 0), size);
        // Weicher blauer Schein hinter dem großen Logo.
        RadialGlow(background, ImVec2(Px(165.0f), Px(285.0f)), ImVec2(Px(250.0f), Px(250.0f)), WithAlpha(kAccent, 0.16f));

        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Kopfzeile = Titelleiste des Fensters.
        m_host.SetCaptionHeight(Px(56.0f));
        const float captionY = Px(28.0f);
        Logo(dl, ImVec2(Px(28.0f) + Px(9.0f), captionY), Px(9.0f));
        const float wordX = Px(28.0f) + Px(18.0f) + Px(10.0f);
        const float wordWidth = Wordmark(dl, ImVec2(wordX, captionY - Measure(GetFonts().light, Px(17.0f), "K").y * 0.5f), Px(17.0f));
        Spaced(dl, GetFonts().brand, Px(10.5f), ImVec2(wordX + wordWidth + Px(12.0f), captionY - Px(6.0f)), kText3, "SETUP",
               0.2f);
        m_host.DrawWindowButtons(size.x - Px(10.0f), captionY);

        // Links: großes Logo mit Version.
        const ImVec2 logoCenter(Px(165.0f), Px(270.0f));
        Logo(dl, logoCenter, Px(74.0f));
        const std::string version = "VERSION " + std::string(KRAKZNCAM_VERSION);
        const float versionWidth = SpacedWidth(GetFonts().brand, Px(10.5f), version, 0.2f);
        Spaced(dl, GetFonts().brand, Px(10.5f), ImVec2(logoCenter.x - versionWidth * 0.5f, logoCenter.y + Px(100.0f)), kText3,
               version, 0.2f);

        // Rechts: Inhalt der Seite, leicht eingeblendet beim Seitenwechsel.
        const float reveal = Ease(std::min(1.0f, std::chrono::duration<float>(Clock::now() - m_pageShown).count() / 0.45f));
        m_animating = reveal < 1.0f;
        PushAlpha(reveal);
        const ImVec2 content(Px(340.0f), Px(112.0f) + (1.0f - reveal) * Px(10.0f));
        const float width = size.x - content.x - Px(44.0f);
        switch (m_page) {
        case Page::Welcome: DrawWelcome(dl, content, width, size); break;
        case Page::Working: DrawWorking(dl, content, width); break;
        case Page::Done: DrawDone(dl, content, width, size); break;
        case Page::Failed: DrawFailed(dl, content, width, size); break;
        case Page::UninstallConfirm: DrawUninstallConfirm(dl, content, width, size); break;
        case Page::Removed: DrawRemoved(dl, content, width, size); break;
        }
        PopAlpha();
        ImGui::End();
        m_animating = m_animating || Theme::Animating();
    }

private:
    using Clock = std::chrono::steady_clock;

    void ShowPage(Page page) {
        if (m_page == page) return;
        m_page = page;
        m_pageShown = Clock::now();
    }

    // Untere Knopfreihe, rechtsbündig. Gibt an, welcher Knopf gedrückt wurde (0 = keiner).
    int Buttons(ImVec2 size, const char* primary, char32_t primaryIcon, const char* secondary) {
        const float y = size.y - Px(44.0f) - ButtonHeight(ButtonSize::Normal);
        float x = size.x - Px(44.0f);
        int clicked = 0;
        const float primaryWidth = ButtonWidth(primary, primaryIcon, ButtonSize::Normal);
        x -= primaryWidth;
        if (Button("##primary", primary, primaryIcon, ImVec2(x, y), ButtonSize::Normal, true)) clicked = 1;
        if (secondary) {
            const float secondaryWidth = ButtonWidth(secondary, 0, ButtonSize::Normal);
            x -= secondaryWidth + Px(12.0f);
            if (Button("##secondary", secondary, 0, ImVec2(x, y))) clicked = 2;
        }
        return clicked;
    }

    float Title(ImDrawList* dl, ImVec2 pos, float width, std::string_view kicker, std::string_view title) {
        float y = pos.y;
        y += Kicker(dl, ImVec2(pos.x, y), kicker) + Px(10.0f);
        y += Text(dl, GetFonts().light, Px(34.0f), ImVec2(pos.x, y), kText, title, width) + Px(12.0f);
        return y;
    }

    float Body(ImDrawList* dl, ImVec2 pos, float width, std::string_view text) {
        return Text(dl, GetFonts().light, Px(16.0f), pos, kText2, text, width);
    }

    void DrawWelcome(ImDrawList* dl, ImVec2 pos, float width, ImVec2 size) {
        const bool installed = !m_installed.empty();
        const int compare = installed ? CompareVersions(KRAKZNCAM_VERSION, m_installed) : 1;
        const char* title = !installed ? "KrakznCam installieren" : compare > 0 ? "KrakznCam aktualisieren" : "KrakznCam neu installieren";
        float y = Title(dl, pos, width, installed ? "UPDATE" : "INSTALLATION", title);
        std::string body = "Macht dein iPhone über das USB-Kabel zur Webcam für Windows, auch für OBS, Discord und Co.";
        if (installed) body = "Installiert ist Version " + m_installed + ", dieses Setup bringt Version " + KRAKZNCAM_VERSION + ".";
        y += Body(dl, ImVec2(pos.x, y), width, body) + Px(22.0f);
        if (!installed) {
            y += OptionRow(dl, "##desktop", ImVec2(pos.x, y), width, "Verknüpfung auf dem Desktop", "", m_desktop) + Px(10.0f);
            if (m_cameraSupported) {
                y += OptionRow(dl, "##camera", ImVec2(pos.x, y), width, "Virtuelle Kamera für OBS",
                               "Windows fragt dafür einmal nach deiner Zustimmung.", m_camera);
            }
        }
        Text(dl, GetFonts().regular, Px(12.5f), ImVec2(pos.x, size.y - Px(44.0f) - ButtonHeight(ButtonSize::Normal) - Px(34.0f)), kText3,
             "Ordner: AppData\\Local\\Programs\\KrakznCam · ohne Administratorrechte", width);
        const int clicked = Buttons(size, installed ? (compare > 0 ? "Aktualisieren" : "Neu installieren") : "Installieren",
                                    Icon::Download, "Abbrechen");
        if (clicked == 1) StartInstall();
        if (clicked == 2) m_close = true;
    }

    void DrawWorking(ImDrawList* dl, ImVec2 pos, float width) {
        std::lock_guard lock(m_mutex);
        const bool uninstall = m_options.mode == Mode::Uninstall;
        const bool update = m_options.mode == Mode::Update;
        float y = Title(dl, pos, width, uninstall ? "DEINSTALLATION" : update ? "UPDATE" : "INSTALLATION",
                        uninstall ? "Wird entfernt …" : update ? "KrakznCam wird aktualisiert …" : "Wird installiert …");
        if (update) {
            const std::string from = m_installed.empty() ? std::string("vorherige Version") : m_installed;
            y += Body(dl, ImVec2(pos.x, y), width, "Von " + from + " auf " + KRAKZNCAM_VERSION + ". KrakznCam startet danach von selbst.") +
                 Px(26.0f);
        } else {
            y += Px(14.0f);
        }
        const float shown = Animate(ImGui::GetID("##progress"), m_progress, 6.0f);
        Meter(dl, ImVec2(pos.x, y), ImVec2(pos.x + width, y + Px(6.0f)), shown);
        y += Px(18.0f);
        Text(dl, GetFonts().regular, Px(14.0f), ImVec2(pos.x, y), kText2, m_step, width);
    }

    void DrawDone(ImDrawList* dl, ImVec2 pos, float width, ImVec2 size) {
        const bool update = m_options.mode == Mode::Update;
        float y = Title(dl, pos, width, update ? "UPDATE" : "INSTALLATION", update ? "Aktualisiert" : "KrakznCam ist bereit");
        std::string body = update ? "KrakznCam startet gleich neu."
                                  : std::string("Du findest KrakznCam im Startmenü") + (m_desktop ? " und auf dem Desktop." : ".");
        y += Body(dl, ImVec2(pos.x, y), width, body) + Px(18.0f);
        if (!m_result.note.empty()) y += Note(dl, ImVec2(pos.x, y), width, m_result.note, false);
        if (update) {
            // Nach kurzer Anzeige: KrakznCam starten und das Setup schließen.
            if (Clock::now() - m_pageShown > std::chrono::milliseconds(1200) && !m_launched) {
                m_launched = true;
                LaunchApp(m_paths);
                m_close = true;
            }
            m_animating = true;
            return;
        }
        if (m_options.automatic) {
            m_close = true;
            return;
        }
        const int clicked = Buttons(size, "KrakznCam starten", Icon::OpenExternal, "Schließen");
        if (clicked == 1) {
            LaunchApp(m_paths);
            m_close = true;
        }
        if (clicked == 2) m_close = true;
    }

    void DrawFailed(ImDrawList* dl, ImVec2 pos, float width, ImVec2 size) {
        float y = Title(dl, pos, width, m_options.mode == Mode::Uninstall ? "DEINSTALLATION" : "INSTALLATION",
                        "Das hat nicht geklappt");
        y += Note(dl, ImVec2(pos.x, y), width, m_result.error, true);
        const int clicked = Buttons(size, "Erneut versuchen", Icon::Refresh, "Schließen");
        if (clicked == 1) {
            if (m_options.mode == Mode::Uninstall) StartUninstall();
            else StartInstall();
        }
        if (clicked == 2) m_close = true;
    }

    void DrawUninstallConfirm(ImDrawList* dl, ImVec2 pos, float width, ImVec2 size) {
        float y = Title(dl, pos, width, "DEINSTALLATION", "KrakznCam entfernen?");
        y += Body(dl, ImVec2(pos.x, y), width,
                  "Entfernt das Programm, die Verknüpfungen, die virtuelle Kamera und die Protokolle. Die App auf dem "
                  "iPhone und Sideloadly bleiben erhalten.") +
             Px(18.0f);
        const int clicked = Buttons(size, "Entfernen", Icon::Cancel, "Abbrechen");
        if (clicked == 1) StartUninstall();
        if (clicked == 2) m_close = true;
    }

    void DrawRemoved(ImDrawList* dl, ImVec2 pos, float width, ImVec2 size) {
        float y = Title(dl, pos, width, "DEINSTALLATION", "KrakznCam wurde entfernt");
        y += Body(dl, ImVec2(pos.x, y), width, "Danke fürs Ausprobieren.") + Px(18.0f);
        if (!m_result.note.empty()) Note(dl, ImVec2(pos.x, y), width, m_result.note, false);
        if (m_options.automatic || Buttons(size, "Schließen", Icon::Check, nullptr) == 1) m_close = true;
    }

    void StartInstall() {
        if (m_working) return;
        if (m_worker.joinable()) m_worker.join();
        m_working = true;
        m_progress = 0.0f;
        m_step = "Bereite vor …";
        ShowPage(Page::Working);
        InstallOptions install;
        install.desktopShortcut = m_desktop;
        install.camera = m_camera;
        install.update = m_options.mode == Mode::Update || !m_installed.empty();
        install.waitPid = m_options.waitPid;
        m_worker = std::thread([this, install] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const TaskResult result = Install(m_paths, install, [this](float fraction, const std::string& step) {
                std::lock_guard lock(m_mutex);
                m_progress = fraction;
                m_step = step;
            });
            CoUninitialize();
            std::lock_guard lock(m_mutex);
            m_pending = result;
            m_finished = true;
        });
    }

    void StartUninstall() {
        if (m_working) return;
        if (m_worker.joinable()) m_worker.join();
        m_working = true;
        m_progress = 0.0f;
        m_step = "Bereite vor …";
        ShowPage(Page::Working);
        m_worker = std::thread([this] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const TaskResult result = Uninstall(m_paths, [this](float fraction, const std::string& step) {
                std::lock_guard lock(m_mutex);
                m_progress = fraction;
                m_step = step;
            });
            CoUninitialize();
            std::lock_guard lock(m_mutex);
            m_pending = result;
            m_finished = true;
        });
    }

    void PollWorker() {
        bool finished = false;
        {
            std::lock_guard lock(m_mutex);
            finished = m_finished;
            if (finished) {
                m_finished = false;
                m_result = m_pending;
            }
        }
        if (!finished) return;
        m_worker.join();
        m_working = false;
        if (!m_result.ok) ShowPage(Page::Failed);
        else ShowPage(m_options.mode == Mode::Uninstall ? Page::Removed : Page::Done);
    }

    // Für Bildschirmfotos: Seite mit Beispieldaten zeigen, ohne etwas zu tun.
    void ApplyPreview() {
        const std::wstring& page = m_options.preview;
        if (page.empty()) return;
        m_pageShown = Clock::now() - std::chrono::seconds(5);
        if (page == L"welcome") {
            m_installed.clear();
            m_page = Page::Welcome;
        } else if (page == L"update") {
            m_installed = "0.0.9";
            m_page = Page::Welcome;
        } else if (page == L"progress") {
            m_options.mode = Mode::Update;
            m_installed = "0.0.9";
            m_page = Page::Working;
            m_progress = 0.55f;
            m_step = "Kopiere die virtuelle Kamera …";
        } else if (page == L"done") {
            m_page = Page::Done;
            m_result.note = "Die virtuelle Kamera ist noch nicht eingerichtet. Das geht jederzeit in KrakznCam unter Einrichtung.";
        } else if (page == L"failed") {
            m_page = Page::Failed;
            m_result.error = "KrakznCam läuft noch. Schließe es bitte und versuche es dann erneut.";
        } else if (page == L"uninstall") {
            m_options.mode = Mode::Uninstall;
            m_page = Page::UninstallConfirm;
        } else if (page == L"removed") {
            m_options.mode = Mode::Uninstall;
            m_page = Page::Removed;
        }
    }

    UiHost& m_host;
    Options m_options;
    InstallPaths m_paths;
    std::string m_installed;
    bool m_cameraSupported = false;
    bool m_desktop = true;
    bool m_camera = true;
    Page m_page = Page::Welcome;
    Clock::time_point m_pageShown = Clock::now();
    bool m_animating = true;
    bool m_close = false;
    bool m_launched = false;

    std::thread m_worker;
    std::atomic<bool> m_working{false};
    std::mutex m_mutex;
    float m_progress = 0.0f;
    std::string m_step;
    bool m_finished = false;
    TaskResult m_pending;
    TaskResult m_result;
};

std::wstring TempPath(const wchar_t* name) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + name;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    Options options = ParseOptions();
    LogInit(TempPath(L"KrakznCam-Setup.log"));
    LogLine("KrakznCam-Setup %s startet", KRAKZNCAM_VERSION);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Die Deinstallation läuft aus einer Kopie im Temp-Ordner, damit sie den Programmordner
    // samt sich selbst löschen kann.
    const InstallPaths paths = MakePaths(options.testRoot);
    if (options.mode == Mode::Uninstall && !options.fromTemp && options.preview.empty() && RunningFromInstallDir(paths)) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring copy = TempPath(L"KrakznCam-Deinstallation.exe");
        if (CopyFileW(self, copy.c_str(), FALSE)) {
            std::wstring command = L"\"" + copy + L"\" --uninstall --from-temp";
            if (options.automatic) command += L" --auto";
            if (!options.testRoot.empty()) command += L" --test-root \"" + options.testRoot + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                CoUninitialize();
                return 0;
            }
        }
    }

    int exitCode = 0;
    {
        UiHost host;
        UiHost::Options window;
        window.instance = instance;
        window.className = L"KrakznCamSetupWindow";
        window.title = L"KrakznCam Setup";
        window.width = 820;
        window.height = 520;
        window.minWidth = 820;
        window.minHeight = 520;
        window.resizable = false;
        window.exactPixelSize = !options.preview.empty();
        if (!host.Create(window)) {
            CoUninitialize();
            return 1;
        }
        SetupApp app(host, options);

        if (!options.preview.empty()) {
            // Bildschirmfoto einer Seite (unsichtbares Fenster).
            for (int i = 0; i < 50; ++i) {
                host.ProcessMessages();
                host.BeginFrame();
                app.Draw();
                host.EndFrame(true, false);
                Sleep(10);
            }
            ComPtr<ID3D11Texture2D> backBuffer;
            if (!options.screenshot.empty() && SUCCEEDED(host.SwapChain()->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
                SaveTexturePng(host.Device(), host.Context(), backBuffer.Get(), options.screenshot);
            }
        } else {
            host.Show(showCommand);
            int extraFrames = 2;
            for (;;) {
                const DWORD timeout = extraFrames > 0 || app.Animating() ? 16 : 250;
                MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                bool hadInput = false;
                if (!host.ProcessMessages(&hadInput)) break;
                if (hadInput) extraFrames = 2;
                if (app.WantsClose()) break;
                if (host.Minimized() || host.Occluded() || !host.ReadyForFrame(20)) continue;
                host.BeginFrame();
                app.Draw();
                host.EndFrame(true);
                if (extraFrames > 0) --extraFrames;
            }
        }
    }
    // Die Temp-Kopie der Deinstallation räumt sich selbst weg.
    if (options.mode == Mode::Uninstall && options.fromTemp) DeleteSelfLater();
    CoUninitialize();
    LogLine("KrakznCam-Setup beendet");
    return exitCode;
}
