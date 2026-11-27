#include "Installer.h"

#include "Log.h"
#include "TextUtil.h"
#include "VCamShared.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kAppExe[] = L"krakzncam.exe";
constexpr wchar_t kIpa[] = L"KrakznCam.ipa";
constexpr wchar_t kUninstaller[] = L"uninstall.exe";
constexpr wchar_t kWindowClass[] = L"KrakznCamWindow";

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

std::wstring OwnPath() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, length);
}

std::wstring Directory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Prozesse, deren Programmdatei genau diese ist.
std::vector<DWORD> ProcessesOf(const std::wstring& exePath) {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const std::wstring wanted = Lower(exePath);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, kAppExe) != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t image[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, image, &size) && Lower(image) == wanted) {
            result.push_back(entry.th32ProcessID);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return result;
}

// Bittet laufende KrakznCam-Fenster aus dem Programmordner, sich zu schließen, und wartet darauf.
bool CloseRunningApp(const InstallPaths& paths, DWORD waitPid) {
    if (waitPid) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (process) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
    }
    const std::wstring exe = paths.installDir + L"\\" + kAppExe;
    std::vector<DWORD> running = ProcessesOf(exe);
    if (running.empty()) return true;
    struct Search {
        std::vector<DWORD>* pids;
    } search{&running};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* data = reinterpret_cast<Search*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            wchar_t className[64];
            GetClassNameW(hwnd, className, 64);
            if (wcscmp(className, kWindowClass) == 0 &&
                std::find(data->pids->begin(), data->pids->end(), pid) != data->pids->end()) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    for (int i = 0; i < 100; ++i) {
        if (ProcessesOf(exe).empty()) return true;
        Sleep(100);
    }
    return false;
}

bool ExtractResource(int id, const std::wstring& target) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(nullptr, resource);
    if (!data || size == 0) return false;
    // Erst daneben schreiben, dann ersetzen: Ein abgebrochenes Setup hinterlässt keine halbe Datei.
    const std::wstring temp = target + L".neu";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool HasResource(int id) {
    return FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA) != nullptr;
}

bool CreateShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& description) {
    ComPtr<IShellLinkW> shell;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
    shell->SetPath(target.c_str());
    shell->SetWorkingDirectory(Directory(target).c_str());
    shell->SetDescription(description.c_str());
    shell->SetIconLocation(target.c_str(), 0);
    ComPtr<IPersistFile> file;
    if (FAILED(shell.As(&file))) return false;
    SHCreateDirectoryExW(nullptr, Directory(link).c_str(), nullptr);
    return SUCCEEDED(file->Save(link.c_str(), TRUE));
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

uint64_t DirectorySize(const std::wstring& directory) {
    uint64_t total = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            total += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return total;
}

// Eintrag unter „Apps“ in den Windows-Einstellungen (nur für diesen Benutzer).
void WriteUninstallEntry(const InstallPaths& paths) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, paths.uninstallKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    const std::wstring exe = paths.installDir + L"\\" + kAppExe;
    SetString(key, L"DisplayName", L"KrakznCam");
    SetString(key, L"DisplayVersion", Wide(KRAKZNCAM_VERSION));
    SetString(key, L"Publisher", L"KrakznCam");
    SetString(key, L"DisplayIcon", exe + L",0");
    SetString(key, L"InstallLocation", paths.installDir);
    SetString(key, L"UninstallString", L"\"" + paths.installDir + L"\\" + kUninstaller + L"\" --uninstall");
    SetString(key, L"URLInfoAbout", L"https://github.com/Kr3akz/Krakzn-Cam");
    SetDword(key, L"NoModify", 1);
    SetDword(key, L"NoRepair", 1);
    SetDword(key, L"EstimatedSize", static_cast<DWORD>(DirectorySize(paths.installDir) / 1024));
    RegCloseKey(key);
}

// Startet ein Programm mit Administratorrechten (UAC-Abfrage) und wartet. -1 = abgelehnt/fehlgeschlagen.
long RunElevated(const std::wstring& exe, const std::wstring& arguments) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        LogLine("Erhöhter Start abgelehnt oder fehlgeschlagen (Fehler %lu)", GetLastError());
        return -1;
    }
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<long>(code);
}

long RunAndWait(const std::wstring& exe, const std::wstring& arguments) {
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<long>(code);
}

std::wstring RegisteredCameraDll() {
    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + VCam::kSourceClsidString + L"\\InprocServer32";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) {
        return {};
    }
    return path;
}

bool SameContent(const std::wstring& a, const std::wstring& b) {
    std::ifstream first(a, std::ios::binary);
    std::ifstream second(b, std::ios::binary);
    if (!first || !second) return false;
    return std::vector<char>(std::istreambuf_iterator<char>(first), {}) ==
           std::vector<char>(std::istreambuf_iterator<char>(second), {});
}

void DeleteDirectoryTree(const std::wstring& directory) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = directory + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteDirectoryTree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(directory.c_str());
}

}  // namespace

InstallPaths MakePaths(const std::wstring& testRoot) {
    InstallPaths paths;
    if (!testRoot.empty()) {
        paths.test = true;
        paths.installDir = testRoot + L"\\Programs\\KrakznCam";
        paths.startMenu = testRoot + L"\\Startmenue\\KrakznCam.lnk";
        paths.desktop = testRoot + L"\\Desktop\\KrakznCam.lnk";
        paths.uninstallKey = L"Software\\KrakznCamSetupTest\\Uninstall\\KrakznCam";
        paths.dataDir = testRoot + L"\\Daten";
        return paths;
    }
    paths.installDir = KnownFolder(FOLDERID_UserProgramFiles) + L"\\KrakznCam";
    paths.startMenu = KnownFolder(FOLDERID_Programs) + L"\\KrakznCam.lnk";
    paths.desktop = KnownFolder(FOLDERID_Desktop) + L"\\KrakznCam.lnk";
    paths.uninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KrakznCam";
    paths.dataDir = KnownFolder(FOLDERID_LocalAppData) + L"\\KrakznCam";
    return paths;
}

std::string InstalledVersion(const InstallPaths& paths) {
    wchar_t version[64];
    DWORD size = sizeof(version);
    if (RegGetValueW(HKEY_CURRENT_USER, paths.uninstallKey.c_str(), L"DisplayVersion", RRF_RT_REG_SZ, nullptr, version,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    if (GetFileAttributesW((paths.installDir + L"\\" + kAppExe).c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return Utf8(version);
}

bool CameraSupported() {
    HMODULE module = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const bool supported = module && GetProcAddress(module, "MFCreateVirtualCamera");
    if (module) FreeLibrary(module);
    return supported;
}

bool RunningFromInstallDir(const InstallPaths& paths) {
    return Lower(Directory(OwnPath())) == Lower(paths.installDir);
}

TaskResult Install(const InstallPaths& paths, const InstallOptions& options, const ProgressFn& progress) {
    TaskResult result;
    progress(0.05f, options.update ? "Warte, bis KrakznCam beendet ist …" : "Bereite die Installation vor …");
    if (!CloseRunningApp(paths, options.waitPid)) {
        result.error = "KrakznCam läuft noch. Schließe es bitte und versuche es dann erneut.";
        return result;
    }
    if (SHCreateDirectoryExW(nullptr, paths.installDir.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(paths.installDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = "Der Programmordner ließ sich nicht anlegen.";
        return result;
    }

    struct Payload {
        int id;
        const wchar_t* name;
        const char* step;
    };
    const Payload payload[] = {
        {101, kAppExe, "Kopiere KrakznCam …"},
        {102, VCam::kDllName, "Kopiere die virtuelle Kamera …"},
        {103, kIpa, "Kopiere die iPhone-App …"},
    };
    float fraction = 0.15f;
    for (const Payload& item : payload) {
        if (!HasResource(item.id)) continue;  // die iPhone-App ist nur in Release-Builds dabei
        progress(fraction, item.step);
        if (!ExtractResource(item.id, paths.installDir + L"\\" + item.name)) {
            LogLine("Setup: %ls ließ sich nicht schreiben (Fehler %lu)", item.name, GetLastError());
            result.error = "Eine Programmdatei ließ sich nicht schreiben. Läuft KrakznCam vielleicht noch?";
            return result;
        }
        fraction += 0.2f;
    }

    // Das Setup selbst dient später als Deinstallationsprogramm.
    progress(0.75f, "Richte Verknüpfungen ein …");
    const std::wstring uninstaller = paths.installDir + L"\\" + kUninstaller;
    if (Lower(OwnPath()) != Lower(uninstaller)) CopyFileW(OwnPath().c_str(), uninstaller.c_str(), FALSE);
    const std::wstring exe = paths.installDir + L"\\" + kAppExe;
    CreateShortcut(paths.startMenu, exe, L"iPhone als Webcam über USB");
    // Beim Update nur eine schon vorhandene Desktop-Verknüpfung erneuern.
    const bool desktopExists = GetFileAttributesW(paths.desktop.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (options.update ? desktopExists : options.desktopShortcut) {
        CreateShortcut(paths.desktop, exe, L"iPhone als Webcam über USB");
    }
    WriteUninstallEntry(paths);

    // Virtuelle Kamera: bei Neuinstallation auf Wunsch, bei Updates nur, wenn sich die DLL geändert hat.
    if (!paths.test && CameraSupported()) {
        const std::wstring registered = RegisteredCameraDll();
        const std::wstring bundled = paths.installDir + L"\\" + VCam::kDllName;
        const bool wanted = options.update ? (!registered.empty() && !SameContent(registered, bundled))
                                           : (options.camera && (registered.empty() || !SameContent(registered, bundled)));
        if (wanted) {
            progress(0.88f, "Windows fragt nach deiner Zustimmung für die virtuelle Kamera …");
            if (RunElevated(exe, L"--install-camera") != 0) {
                result.note = "Die virtuelle Kamera ist noch nicht eingerichtet. Das geht jederzeit in KrakznCam unter "
                              "Einrichtung.";
            }
        }
    }
    progress(1.0f, "Fertig");
    LogLine("Setup: Version %s installiert nach %ls", KRAKZNCAM_VERSION, paths.installDir.c_str());
    result.ok = true;
    return result;
}

TaskResult Uninstall(const InstallPaths& paths, const ProgressFn& progress) {
    TaskResult result;
    progress(0.05f, "Schließe KrakznCam …");
    if (!CloseRunningApp(paths, 0)) {
        result.error = "KrakznCam läuft noch. Schließe es bitte und versuche es dann erneut.";
        return result;
    }
    const std::wstring exe = paths.installDir + L"\\" + kAppExe;
    const bool haveExe = GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!paths.test && haveExe) {
        // Hatte KrakznCam den Autostart von Sideloadly übernommen, bekommt Sideloadly ihn zurück.
        progress(0.2f, "Gebe Sideloadly seinen Autostart zurück …");
        RunAndWait(exe, L"--cleanup");
        if (!RegisteredCameraDll().empty()) {
            progress(0.35f, "Windows fragt nach deiner Zustimmung, um die virtuelle Kamera zu entfernen …");
            if (RunElevated(exe, L"--uninstall-camera") != 0) {
                result.note = "Die virtuelle Kamera blieb eingerichtet, weil Windows nicht zugestimmt wurde.";
            }
        }
    }
    progress(0.6f, "Entferne Programmdateien …");
    DeleteFileW(paths.startMenu.c_str());
    DeleteFileW(paths.desktop.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, paths.uninstallKey.c_str());
    if (paths.test) RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\KrakznCamSetupTest");
    DeleteDirectoryTree(paths.installDir);
    progress(0.85f, "Entferne Protokolle und Einstellungen …");
    DeleteDirectoryTree(paths.dataDir);
    progress(1.0f, "Fertig");
    LogLine("Setup: KrakznCam entfernt");
    result.ok = true;
    return result;
}

bool LaunchApp(const InstallPaths& paths) {
    const std::wstring exe = paths.installDir + L"\\" + kAppExe;
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, paths.installDir.c_str(),
                        &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void DeleteSelfLater() {
    // cmd wartet zwei Sekunden (bis das Setup beendet ist) und löscht dann die Datei.
    const std::wstring command = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + OwnPath() + L"\"";
    std::wstring line = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
