#include "SetupCheck.h"

#include "Log.h"
#include "TextUtil.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace {

// Dieselben Direktlinks wie die Download-Knöpfe auf sideloadly.io. Beide Dateien liegen bei Apple.
constexpr wchar_t kITunesUrl[] = L"https://www.apple.com/itunes/download/win64";
constexpr wchar_t kICloudUrl[] =
    L"https://updates.cdn-apple.com/2020/windows/001-39935-20200911-1A70AA56-F448-11EA-8CC0-99D41950005E/"
    L"iCloudSetup.exe";
constexpr wchar_t kSideloadlyUrl[] = L"https://sideloadly.io";
constexpr wchar_t kAppsSettings[] = L"ms-settings:appsfeatures";

struct InstalledApp {
    std::wstring name;
    std::wstring version;
    std::wstring displayIcon;
    std::wstring installLocation;
};

std::wstring ReadString(HKEY parent, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(parent, subkey, value, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) return buffer;
    return {};
}

std::vector<InstalledApp> ListInstalledApps() {
    std::vector<InstalledApp> apps;
    struct Root {
        HKEY key;
        REGSAM view;
    };
    const Root roots[] = {
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
        {HKEY_CURRENT_USER, 0},
    };
    for (const Root& root : roots) {
        HKEY uninstall = nullptr;
        if (RegOpenKeyExW(root.key, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                          KEY_READ | root.view, &uninstall) != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t subkey[256];
            DWORD length = 256;
            if (RegEnumKeyExW(uninstall, index, subkey, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            InstalledApp app;
            app.name = ReadString(uninstall, subkey, L"DisplayName");
            if (app.name.empty()) continue;
            app.version = ReadString(uninstall, subkey, L"DisplayVersion");
            app.displayIcon = ReadString(uninstall, subkey, L"DisplayIcon");
            app.installLocation = ReadString(uninstall, subkey, L"InstallLocation");
            apps.push_back(std::move(app));
        }
        RegCloseKey(uninstall);
    }
    return apps;
}

// Apps aus dem Microsoft Store tauchen nicht unter „Uninstall“ auf, aber im Paket-Verzeichnis des Benutzers.
std::vector<std::wstring> ListStorePackages() {
    std::vector<std::wstring> packages;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\"
                      L"Repository\\Packages",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return packages;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[512];
        DWORD length = 512;
        if (RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        packages.emplace_back(name);
    }
    RegCloseKey(key);
    return packages;
}

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool Contains(const std::wstring& text, const wchar_t* part) {
    std::wstring a = text;
    std::wstring b = part;
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    return a.find(b) != std::wstring::npos;
}

const InstalledApp* FindApp(const std::vector<InstalledApp>& apps, bool (*match)(const std::wstring&)) {
    for (const InstalledApp& app : apps) {
        if (match(app.name)) return &app;
    }
    return nullptr;
}

bool HasPackage(const std::vector<std::wstring>& packages, const wchar_t* prefix) {
    for (const std::wstring& package : packages) {
        if (StartsWith(package, prefix)) return true;
    }
    return false;
}

enum class ServiceState { Missing, Stopped, Running };

ServiceState QueryServiceState(const wchar_t* name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return ServiceState::Missing;
    ServiceState state = ServiceState::Missing;
    if (SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        state = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING
                    ? ServiceState::Running
                    : ServiceState::Stopped;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return state;
}

DWORD WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion && rtlGetVersion(&info) == 0) return info.dwBuildNumber;
    return 0;
}

std::wstring FindExecutable(const InstalledApp& app, const wchar_t* exeName) {
    std::wstring icon = app.displayIcon;
    if (!icon.empty()) {
        if (icon.front() == L'"') {
            icon.erase(0, 1);
            size_t quote = icon.find(L'"');
            if (quote != std::wstring::npos) icon.erase(quote);
        }
        size_t comma = icon.rfind(L',');
        if (comma != std::wstring::npos && comma > 2) icon.erase(comma);
        if (Contains(icon, L".exe") && GetFileAttributesW(icon.c_str()) != INVALID_FILE_ATTRIBUTES) return icon;
    }
    if (!app.installLocation.empty()) {
        std::wstring path = app.installLocation;
        if (path.back() != L'\\') path += L'\\';
        path += exeName;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Programmpfad aus einer Befehlszeile wie "\"D:\\x\\y.exe\" --arg" oder "D:\\x\\y.exe".
std::wstring ExecutableOf(std::wstring command) {
    if (!command.empty() && command.front() == L'"') {
        const size_t quote = command.find(L'"', 1);
        return command.substr(1, quote == std::wstring::npos ? std::wstring::npos : quote - 1);
    }
    std::wstring lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const size_t exe = lower.find(L".exe");
    return exe == std::wstring::npos ? command : command.substr(0, exe + 4);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) path = folder;
    CoTaskMemFree(folder);
    return path;
}

std::wstring ResolveShortcut(const std::wstring& link) {
    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) ||
        FAILED(shellLink.As(&file)) || FAILED(file->Load(link.c_str(), STGM_READ))) {
        return {};
    }
    wchar_t target[MAX_PATH] = {};
    if (FAILED(shellLink->GetPath(target, MAX_PATH, nullptr, 0))) return {};
    return target;
}

// Pfad eines laufenden Prozesses (leer, wenn er nicht läuft).
std::wstring RunningProcessPath(const wchar_t* exeName) {
    std::wstring result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exeName) != 0) continue;
        result = exeName;  // läuft; den vollen Pfad gibt es nur mit Zugriff auf den Prozess
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) result = path;
            CloseHandle(process);
        }
        break;
    }
    CloseHandle(snapshot);
    return result;
}

// Autostart-Eintrag des Sideloadly-Hintergrunddienstes (erneuert die Signaturen).
std::wstring SideloadlyDaemonAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    std::wstring result;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLength = 256;
        BYTE data[2048];
        DWORD dataLength = sizeof(data) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegEnumValueW(key, index, name, &nameLength, nullptr, &type, data, &dataLength) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dataLength] = 0;
        data[dataLength + 1] = 0;
        const std::wstring command(reinterpret_cast<const wchar_t*>(data));
        if (Contains(command, L"sideloadlydaemon")) {
            result = ExecutableOf(command);
            break;
        }
    }
    RegCloseKey(key);
    return result;
}

// Der Sideloadly-Installer trägt sich nicht in die Programmliste ein und lässt sich frei
// installieren (z. B. D:\Sideloadly). Deshalb an mehreren Stellen suchen.
std::wstring FindSideloadly(const InstalledApp* entry, const std::wstring& daemonAutostart) {
    std::vector<std::wstring> candidates;
    if (entry) candidates.push_back(FindExecutable(*entry, L"sideloadly.exe"));
    if (!daemonAutostart.empty()) candidates.push_back(FolderOf(daemonAutostart) + L"\\sideloadly.exe");
    for (const wchar_t* exe : {L"sideloadly.exe", L"sideloadlydaemon.exe"}) {
        const std::wstring running = RunningProcessPath(exe);
        if (running.find(L'\\') != std::wstring::npos) candidates.push_back(FolderOf(running) + L"\\sideloadly.exe");
    }
    for (const std::wstring& programs : {KnownFolder(FOLDERID_Programs), KnownFolder(FOLDERID_CommonPrograms)}) {
        if (programs.empty()) continue;
        std::error_code error;
        namespace fs = std::filesystem;
        for (fs::recursive_directory_iterator it(programs, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            const fs::path& path = it->path();
            if (path.extension() == L".lnk" && Contains(path.filename().wstring(), L"sideloadly")) {
                candidates.push_back(ResolveShortcut(path.wstring()));
            }
        }
    }
    for (const std::wstring& base : {KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\Sideloadly",
                                     KnownFolder(FOLDERID_ProgramFiles) + L"\\Sideloadly",
                                     KnownFolder(FOLDERID_ProgramFilesX86) + L"\\Sideloadly",
                                     KnownFolder(FOLDERID_LocalAppData) + L"\\Sideloadly"}) {
        candidates.push_back(base + L"\\sideloadly.exe");
    }
    for (const std::wstring& candidate : candidates) {
        if (Contains(candidate, L"sideloadly.exe") && FileExists(candidate)) return candidate;
    }
    return {};
}

// Die KrakznCam-App für das iPhone (.ipa) liegt neben krakzncam.exe oder einen Ordner darüber.
std::wstring FindAppPackage() {
    wchar_t module[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, module, MAX_PATH)) return {};
    namespace fs = std::filesystem;
    const fs::path folder = fs::path(module).parent_path();
    for (const fs::path& search : {folder, folder.parent_path()}) {
        std::error_code error;
        for (fs::directory_iterator it(search, error), end; !error && it != end; it.increment(error)) {
            std::wstring name = it->path().filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (StartsWith(name, L"krakzncam") && it->path().extension() == L".ipa") return it->path().wstring();
        }
    }
    return {};
}

}  // namespace

void RevealInExplorer(const std::wstring& path) {
    const std::wstring arguments = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

const char* SetupStatusName(SetupItem::Status status) {
    switch (status) {
    case SetupItem::Status::Ok: return "OK";
    case SetupItem::Status::Missing: return "FEHLT";
    case SetupItem::Status::Warning: return "WARNUNG";
    case SetupItem::Status::Info: return "INFO";
    }
    return "?";
}

void OpenTarget(const std::wstring& target) {
    LogLine("Öffne: %s", Utf8(target).c_str());
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

SetupReport RunSetupCheck() {
    SetupReport report;
    const std::vector<InstalledApp> apps = ListInstalledApps();
    const std::vector<std::wstring> packages = ListStorePackages();

    // Windows-Version: Die virtuelle Kamera braucht Windows 11 (Build 22000+).
    // Nur erwähnen, wenn es ein Problem ist.
    report.windowsBuild = WindowsBuild();
    if (report.windowsBuild > 0 && report.windowsBuild < 22000) {
        SetupItem item;
        item.key = "windows";
        item.title = "Windows 11";
        item.status = SetupItem::Status::Warning;
        item.detail = "Die virtuelle Kamera braucht Windows 11. Die Vorschau funktioniert trotzdem.";
        report.items.push_back(item);
    }

    // iTunes (Web-Version). Bringt den USB-Treiber samt Apple-USB-Dienst mit, deshalb ein Punkt.
    const InstalledApp* iTunes = FindApp(apps, [](const std::wstring& n) { return n == L"iTunes"; });
    const bool iTunesStore = HasPackage(packages, L"AppleInc.iTunes_");
    const ServiceState usbService = QueryServiceState(L"Apple Mobile Device Service");
    bool iTunesOk = false;
    {
        SetupItem item;
        item.key = "itunes";
        item.title = "iTunes (Web-Version)";
        if (iTunes && usbService == ServiceState::Running) {
            item.status = SetupItem::Status::Ok;
            item.detail = "Version " + Utf8(iTunes->version) + ", USB-Treiber läuft";
            iTunesOk = true;
        } else if (iTunes) {
            item.status = SetupItem::Status::Warning;
            item.detail = "Installiert, aber der Apple-USB-Dienst läuft nicht. Ein Neustart des PCs hilft meistens.";
            item.actionLabel = "Dienste öffnen";
            item.actionTarget = L"services.msc";
            iTunesOk = true;
        } else if (iTunesStore) {
            item.status = SetupItem::Status::Warning;
            item.detail = "Die Microsoft-Store-Version funktioniert nicht mit Sideloadly. Bitte deinstallieren "
                          "und die Web-Version installieren.";
            item.actionLabel = "Store-Version entfernen";
            item.actionTarget = kAppsSettings;
        } else {
            item.status = SetupItem::Status::Missing;
            item.detail = "Bringt den USB-Treiber mit und wird von Sideloadly gebraucht.";
            item.actionLabel = "iTunes herunterladen";
            item.actionTarget = kITunesUrl;
        }
        report.items.push_back(item);
    }

    // iCloud (Web-Version)
    const InstalledApp* iCloud = FindApp(apps, [](const std::wstring& n) {
        return n == L"iCloud" || (StartsWith(n, L"iCloud") && !Contains(n, L"Outlook"));
    });
    const bool iCloudStore = HasPackage(packages, L"AppleInc.iCloud_");
    bool iCloudOk = false;
    {
        SetupItem item;
        item.key = "icloud";
        item.title = "iCloud (Web-Version)";
        if (iCloud) {
            item.status = SetupItem::Status::Ok;
            item.detail = "Version " + Utf8(iCloud->version) + ". Anmelden ist nicht nötig.";
            iCloudOk = true;
        } else if (iCloudStore) {
            item.status = SetupItem::Status::Warning;
            item.detail = "Die Microsoft-Store-Version funktioniert nicht mit Sideloadly. Bitte deinstallieren "
                          "und die Web-Version installieren.";
            item.actionLabel = "Store-Version entfernen";
            item.actionTarget = kAppsSettings;
        } else {
            item.status = SetupItem::Status::Missing;
            item.detail = "Wird von Sideloadly gebraucht. Anmelden musst du dich nicht.";
            item.actionLabel = "iCloud herunterladen";
            item.actionTarget = kICloudUrl;
        }
        report.items.push_back(item);
    }


    // Sideloadly und sein Hintergrunddienst, der die Signatur automatisch erneuert.
    const InstalledApp* sideloadlyEntry =
        FindApp(apps, [](const std::wstring& n) { return Contains(n, L"Sideloadly"); });
    const std::wstring daemonAutostart = SideloadlyDaemonAutostart();
    report.sideloadlyExe = FindSideloadly(sideloadlyEntry, daemonAutostart);
    report.sideloadlyDaemonAutostart = !daemonAutostart.empty();
    report.sideloadlyDaemonRunning = !RunningProcessPath(L"sideloadlydaemon.exe").empty();
    report.appPackage = FindAppPackage();
    {
        SetupItem item;
        item.key = "sideloadly";
        item.title = "Sideloadly";
        if (!report.sideloadlyExe.empty()) {
            item.status = SetupItem::Status::Ok;
            if (report.sideloadlyDaemonRunning) {
                item.detail = "Erneuert die Signatur automatisch, solange sein Hintergrunddienst läuft (läuft gerade).";
            } else if (report.sideloadlyDaemonAutostart) {
                item.detail = "Erneuert die Signatur automatisch. Sein Hintergrunddienst startet mit Windows.";
            } else {
                item.detail = "Bringt die App aufs iPhone. Automatisches Erneuern ist aus, also alle 7 Tage neu "
                              "aufspielen.";
            }
            item.actionLabel = "Öffnen";
            item.actionTarget = report.sideloadlyExe;
        } else {
            item.status = SetupItem::Status::Missing;
            item.detail = "Bringt die KrakznCam-App auf das iPhone.";
            item.actionLabel = "Sideloadly herunterladen";
            item.actionTarget = kSideloadlyUrl;
        }
        report.items.push_back(item);
    }

    // Apple-Geräte-App aus dem Store: kann sich mit iTunes in die Quere kommen.
    if (HasPackage(packages, L"AppleInc.AppleDevices_")) {
        SetupItem item;
        item.key = "appledevices";
        item.status = SetupItem::Status::Info;
        item.title = "„Apple Geräte“-App";
        item.detail = "Aus dem Microsoft Store installiert. Falls Sideloadly das iPhone nicht findet, diese App "
                      "deinstallieren.";
        item.actionLabel = "Apps öffnen";
        item.actionTarget = kAppsSettings;
        report.items.push_back(item);
    }

    report.sideloadReady = iTunesOk && iCloudOk && !report.sideloadlyExe.empty();
    return report;
}
