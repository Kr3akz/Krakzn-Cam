#include "SetupCheck.h"

#include "Log.h"
#include "TextUtil.h"

#include <shellapi.h>

#include <algorithm>
#include <cwctype>

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

}  // namespace

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
    report.windowsBuild = WindowsBuild();
    {
        SetupItem item;
        item.key = "windows";
        item.title = "Windows";
        if (report.windowsBuild >= 22000) {
            item.status = SetupItem::Status::Ok;
            item.detail = "Windows 11 (Build " + std::to_string(report.windowsBuild) + ")";
        } else {
            item.status = SetupItem::Status::Warning;
            item.detail = "Die virtuelle Kamera braucht Windows 11.";
        }
        report.items.push_back(item);
    }

    // iTunes (Web-Version). Bringt den USB-Treiber mit.
    const InstalledApp* iTunes = FindApp(apps, [](const std::wstring& n) { return n == L"iTunes"; });
    const bool iTunesStore = HasPackage(packages, L"AppleInc.iTunes_");
    bool iTunesOk = false;
    {
        SetupItem item;
        item.key = "itunes";
        item.title = "iTunes (Web-Version)";
        if (iTunes) {
            item.status = SetupItem::Status::Ok;
            item.detail = "Version " + Utf8(iTunes->version);
            iTunesOk = true;
        } else if (iTunesStore) {
            item.status = SetupItem::Status::Warning;
            item.detail = "Die Microsoft-Store-Version funktioniert nicht mit Sideloadly. Bitte deinstallieren "
                          "und die Web-Version installieren.";
            item.actionLabel = "Store-Version entfernen";
            item.actionTarget = kAppsSettings;
        } else {
            item.status = SetupItem::Status::Missing;
            item.detail = "Wird von Sideloadly gebraucht und bringt den USB-Treiber mit.";
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

    // USB-Treiber und Dienst (kommen mit iTunes).
    {
        SetupItem item;
        item.key = "usb";
        item.title = "Apple-USB-Dienst";
        switch (QueryServiceState(L"Apple Mobile Device Service")) {
        case ServiceState::Running:
            item.status = SetupItem::Status::Ok;
            item.detail = "Läuft. Darüber spricht KabelCam per Kabel mit dem iPhone.";
            break;
        case ServiceState::Stopped:
            item.status = SetupItem::Status::Warning;
            item.detail = "Installiert, läuft aber nicht. Neustart des PCs hilft meistens.";
            item.actionLabel = "Dienste öffnen";
            item.actionTarget = L"services.msc";
            break;
        case ServiceState::Missing:
            item.status = SetupItem::Status::Missing;
            item.detail = "Wird zusammen mit iTunes installiert.";
            break;
        }
        report.items.push_back(item);
    }

    // Sideloadly
    const InstalledApp* sideloadly = FindApp(apps, [](const std::wstring& n) { return Contains(n, L"Sideloadly"); });
    {
        SetupItem item;
        item.key = "sideloadly";
        item.title = "Sideloadly";
        if (sideloadly) {
            item.status = SetupItem::Status::Ok;
            item.detail = sideloadly->version.empty() ? "Installiert" : "Version " + Utf8(sideloadly->version);
            report.sideloadlyExe = FindExecutable(*sideloadly, L"sideloadly.exe");
            if (!report.sideloadlyExe.empty()) {
                item.actionLabel = "Öffnen";
                item.actionTarget = report.sideloadlyExe;
            }
        } else {
            item.status = SetupItem::Status::Missing;
            item.detail = "Bringt die KabelCam-App auf das iPhone.";
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

    report.sideloadReady = iTunesOk && iCloudOk && sideloadly != nullptr;
    return report;
}
