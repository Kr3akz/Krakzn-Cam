#include "AutoRenew.h"

#include "Log.h"
#include "TextUtil.h"

#include <shlobj.h>

#include <algorithm>
#include <ctime>
#include <cwctype>

namespace {

constexpr wchar_t kSection[] = L"Signatur";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kDaemonExe[] = L"sideloadlydaemon.exe";

std::wstring IniPath() {
    PWSTR folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        path = folder;
        path += L"\\KrakznCam";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\krakzncam.ini";
    }
    CoTaskMemFree(folder);
    return path;
}

std::wstring ReadIni(const std::wstring& ini, const wchar_t* key) {
    wchar_t buffer[1024] = {};
    GetPrivateProfileStringW(kSection, key, L"", buffer, 1024, ini.c_str());
    return buffer;
}

bool ContainsDaemon(const std::wstring& text) {
    std::wstring lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower.find(L"sideloadlydaemon") != std::wstring::npos;
}

// Bittet die Fenster eines Prozesses, sich zu schließen (räumt z. B. das Symbol im Infobereich auf).
void CloseWindowsOf(DWORD processId) {
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == static_cast<DWORD>(parameter)) PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        },
        static_cast<LPARAM>(processId));
}

}  // namespace

AutoRenew::AutoRenew(bool active) : m_iniPath(IniPath()), m_active(active) {
    if (m_iniPath.empty()) return;
    const std::wstring enabled = ReadIni(m_iniPath, L"AutomatischErneuern");
    m_enabled = enabled.empty() || enabled != L"0";
    m_expiry = _wtoi64(ReadIni(m_iniPath, L"Ablauf").c_str());
}

AutoRenew::~AutoRenew() {
    StopDaemon();
}

void AutoRenew::Save() {
    if (m_iniPath.empty() || !m_active) return;
    WritePrivateProfileStringW(kSection, L"AutomatischErneuern", m_enabled ? L"1" : L"0", m_iniPath.c_str());
    WritePrivateProfileStringW(kSection, L"Ablauf", std::to_wstring(m_expiry).c_str(), m_iniPath.c_str());
}

void AutoRenew::SetEnabled(bool enabled) {
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    Save();
    LogLine("Automatisch erneuern: %s", enabled ? "an" : "aus");
    if (!m_active) return;
    if (enabled) {
        RemoveSideloadlyAutostart();
    } else {
        StopDaemon();
        RestoreSideloadlyAutostart();
    }
    m_lastCheck = {};
}

void AutoRenew::Update(int64_t signatureExpires, const SetupReport& setup) {
    if (signatureExpires > 0 && signatureExpires != m_expiry) {
        m_expiry = signatureExpires;
        Save();
        LogLine("Signatur gültig bis %lld (Unix-Zeit)", static_cast<long long>(m_expiry));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastCheck < std::chrono::seconds(2)) return;
    m_lastCheck = now;

    // Hat jemand unseren Dienst inzwischen selbst beendet?
    if (m_daemon && WaitForSingleObject(m_daemon, 0) == WAIT_OBJECT_0) {
        CloseHandle(m_daemon);
        m_daemon = nullptr;
    }
    if (!m_enabled) {
        m_state = State::Off;
        return;
    }
    if (setup.sideloadlyExe.empty()) {
        m_state = State::NoSideloadly;
        return;
    }
    // Einmal pro Start: Sideloadly trägt seinen Autostart beim Aufspielen gern wieder ein.
    if (m_active && !m_autostartChecked) {
        m_autostartChecked = true;
        RemoveSideloadlyAutostart();
    }

    const int64_t remaining = m_expiry - static_cast<int64_t>(time(nullptr));
    const bool due = m_expiry > 0 && remaining < kRenewBefore;
    if (due) {
        if (m_active && !m_daemon && !IsProcessRunning(kDaemonExe)) {
            const std::wstring folder = setup.sideloadlyExe.substr(0, setup.sideloadlyExe.find_last_of(L"\\/"));
            StartDaemon(folder + L"\\" + kDaemonExe);
        }
        m_state = State::Renewing;
    } else {
        if (m_daemon) {
            LogLine("Signatur ist erneuert, beende den Sideloadly-Dienst wieder");
            StopDaemon();
            RemoveSideloadlyAutostart();  // falls er sich beim Erneuern wieder eingetragen hat
        }
        m_state = State::Waiting;
    }
}

void AutoRenew::StartDaemon(const std::wstring& exe) {
    const std::wstring folder = exe.substr(0, exe.find_last_of(L"\\/"));
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                       &process)) {
        CloseHandle(process.hThread);
        m_daemon = process.hProcess;
        LogLine("Signatur läuft bald ab: Sideloadly-Dienst gestartet (%s)", Utf8(exe).c_str());
    } else {
        LogLine("Sideloadly-Dienst konnte nicht gestartet werden (Fehler %lu)", GetLastError());
    }
}

void AutoRenew::StopDaemon() {
    if (!m_daemon) return;
    if (WaitForSingleObject(m_daemon, 0) == WAIT_TIMEOUT) {
        CloseWindowsOf(GetProcessId(m_daemon));
        if (WaitForSingleObject(m_daemon, 1500) == WAIT_TIMEOUT) TerminateProcess(m_daemon, 0);
    }
    CloseHandle(m_daemon);
    m_daemon = nullptr;
}

void AutoRenew::RemoveSideloadlyAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    std::wstring name;
    std::wstring command;
    for (DWORD index = 0; name.empty(); ++index) {
        wchar_t valueName[256];
        DWORD nameLength = 256;
        BYTE data[2048];
        DWORD dataLength = sizeof(data) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegEnumValueW(key, index, valueName, &nameLength, nullptr, &type, data, &dataLength) != ERROR_SUCCESS) {
            break;
        }
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dataLength] = 0;
        data[dataLength + 1] = 0;
        const std::wstring value(reinterpret_cast<const wchar_t*>(data));
        if (ContainsDaemon(value)) {
            name = valueName;
            command = value;
        }
    }
    if (!name.empty() && RegDeleteValueW(key, name.c_str()) == ERROR_SUCCESS) {
        // Merken, damit er beim Ausschalten der Funktion zurückkommt.
        WritePrivateProfileStringW(kSection, L"SideloadlyAutostartName", name.c_str(), m_iniPath.c_str());
        WritePrivateProfileStringW(kSection, L"SideloadlyAutostartBefehl", command.c_str(), m_iniPath.c_str());
        LogLine("Windows-Autostart des Sideloadly-Dienstes entfernt (%s)", Utf8(name).c_str());
    }
    RegCloseKey(key);
}

void AutoRenew::RestoreSideloadlyAutostart() {
    const std::wstring name = ReadIni(m_iniPath, L"SideloadlyAutostartName");
    const std::wstring command = ReadIni(m_iniPath, L"SideloadlyAutostartBefehl");
    if (name.empty() || command.empty()) return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    const LONG result = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result == ERROR_SUCCESS) {
        WritePrivateProfileStringW(kSection, L"SideloadlyAutostartName", nullptr, m_iniPath.c_str());
        WritePrivateProfileStringW(kSection, L"SideloadlyAutostartBefehl", nullptr, m_iniPath.c_str());
        LogLine("Windows-Autostart des Sideloadly-Dienstes wiederhergestellt");
    }
}
