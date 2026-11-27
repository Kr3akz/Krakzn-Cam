#include "VirtualCamera.h"

#include "Log.h"

#include <windows.h>
#include <aclapi.h>
#include <avrt.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using CreateVirtualCameraFn = HRESULT(WINAPI*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
                                                LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);

// MFCreateVirtualCamera gibt es erst ab Windows 11; dynamisch laden, damit KrakznCam
// auch unter Windows 10 startet.
CreateVirtualCameraFn LoadCreateFunction() {
    static const CreateVirtualCameraFn function = [] {
        HMODULE module = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(module, "MFCreateVirtualCamera")) : nullptr;
    }();
    return function;
}

// Einstellungen für alle Benutzer (vom Einrichten mit Administratorrechten geschrieben).
constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\KrakznCam";

std::wstring ClsidKey() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + VCam::kSourceClsidString + L"\\InprocServer32";
}

// Pfad der eingerichteten Kamera-DLL laut Registry (leer = nicht eingerichtet).
std::wstring RegisteredDllPath() {
    wchar_t path[MAX_PATH];
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, ClsidKey().c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) !=
        ERROR_SUCCESS) {
        return {};
    }
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? std::wstring(path) : std::wstring();
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
}

std::wstring BundledDllPath() {
    return ExeDirectory() + L"\\" + VCam::kDllName;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring InstallDirectory() {
    return KnownFolder(FOLDERID_ProgramFiles) + L"\\KrakznCam";
}

bool SameFileContent(const std::wstring& a, const std::wstring& b) {
    std::ifstream first(a, std::ios::binary);
    std::ifstream second(b, std::ios::binary);
    if (!first || !second) return false;
    const std::vector<char> left((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
    const std::vector<char> right((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
    return left == right;
}

HRESULT CallDllFunction(const std::wstring& dll, const char* name) {
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    using Function = HRESULT(STDAPICALLTYPE*)();
    const auto function = reinterpret_cast<Function>(GetProcAddress(module, name));
    const HRESULT hr = function ? function() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    FreeLibrary(module);
    return hr;
}

// Eine geladene DLL lässt sich nicht überschreiben, aber umbenennen; die alte Fassung wird
// beim nächsten Neustart gelöscht.
bool ReplaceFile(const std::wstring& source, const std::wstring& target) {
    if (CopyFileW(source.c_str(), target.c_str(), FALSE)) return true;
    const DWORD error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_USER_MAPPED_FILE) {
        LogLine("Kamera-DLL konnte nicht kopiert werden (Fehler %lu)", error);
        return false;
    }
    const std::wstring old = target + L".alt" + std::to_wstring(GetTickCount64());
    if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LogLine("Alte Kamera-DLL ist in Benutzung und lässt sich nicht umbenennen (Fehler %lu)", GetLastError());
        return false;
    }
    MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
}

// Ordner für das Protokoll der Kamera (der Frame-Server-Dienst darf hineinschreiben).
void CreateLogDirectory() {
    const std::wstring path = KnownFolder(FOLDERID_ProgramData) + L"\\KrakznCam";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1301bf;;;AU)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        CreateDirectoryW(path.c_str(), nullptr);
        return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    CreateDirectoryW(path.c_str(), &security);
    LocalFree(descriptor);
}

bool PersistentFlag() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"DauerhafteKamera", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
               ERROR_SUCCESS &&
           value != 0;
}

// Meldet die Kamera dauerhaft für alle Benutzer an (braucht Administratorrechte). Danach findet
// OBS sie immer, auch wenn KrakznCam geschlossen ist. Nur einmal: Ein zweites Anmelden ergäbe
// zwei Kameras mit gleichem Namen.
void RegisterPersistentCamera() {
    if (PersistentFlag()) {
        LogLine("Einrichtung: Kamera ist schon dauerhaft angemeldet");
        return;
    }
    const CreateVirtualCameraFn create = LoadCreateFunction();
    if (!create) return;
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                        MFVirtualCameraAccess_AllUsers, VCam::kFriendlyName, VCam::kSourceClsidString, nullptr, 0, &camera);
    if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
    if (camera) camera->Shutdown();  // Shutdown entfernt sie nicht, nur Remove täte das
    LogLine("Einrichtung: dauerhafte Anmeldung der Kamera 0x%08X", static_cast<unsigned>(hr));
    if (SUCCEEDED(hr)) {
        const DWORD one = 1;
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"DauerhafteKamera", REG_DWORD, &one, sizeof(one));
    }
}

void RemovePersistentCamera() {
    if (!PersistentFlag()) return;
    const CreateVirtualCameraFn create = LoadCreateFunction();
    if (create) {
        // Mit denselben Angaben bekommt man die bestehende Kamera und kann sie entfernen.
        ComPtr<IMFVirtualCamera> camera;
        HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                            MFVirtualCameraAccess_AllUsers, VCam::kFriendlyName, VCam::kSourceClsidString, nullptr, 0,
                            &camera);
        if (SUCCEEDED(hr)) hr = camera->Remove();
        if (camera) camera->Shutdown();
        LogLine("Kamera dauerhaft abgemeldet (0x%08X)", static_cast<unsigned>(hr));
    }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"DauerhafteKamera");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kSettingsKey);
}

}  // namespace

VirtualCamera::VirtualCamera(ID3D11Device* device, FrameStore& store) : m_store(store), m_feeder(device, store) {
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Anfangszustand für die Oberfläche, auch wenn die Kamera nicht gestartet wird (Selbsttest).
    RefreshInstallState();
    std::lock_guard lock(m_mutex);
    m_status.state = !LoadCreateFunction() ? State::Unsupported : !m_installed ? State::NotInstalled : State::Starting;
}

VirtualCamera::~VirtualCamera() {
    Stop();
    if (m_installThread.joinable()) m_installThread.join();
    CloseHandle(m_frameEvent);
    CloseHandle(m_stopEvent);
    CloseHandle(m_wakeEvent);
}

void VirtualCamera::Start(bool registerCamera) {
    if (m_thread.joinable()) return;
    m_register = registerCamera;
    // Solange es diese Markierung gibt, empfängt die Kamera-DLL das iPhone nicht selbst,
    // sondern nimmt die Bilder von KrakznCam (sonst stritten beide um die Verbindung).
    if (!m_appMutex) m_appMutex = CreateMutexW(nullptr, FALSE, VCam::AppMutexName().c_str());
    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { Run(); });
}

void VirtualCamera::Stop() {
    if (m_thread.joinable()) {
        SetEvent(m_stopEvent);
        m_thread.join();
    }
    if (m_appMutex) {
        CloseHandle(m_appMutex);
        m_appMutex = nullptr;
    }
}

VirtualCamera::Status VirtualCamera::GetStatus() {
    std::lock_guard lock(m_mutex);
    return m_status;
}

void VirtualCamera::SetState(State state, const std::string& detail) {
    std::lock_guard lock(m_mutex);
    if (m_status.state != state || m_status.detail != detail) {
        LogLine("Virtuelle Kamera: Zustand %d %s", static_cast<int>(state), detail.c_str());
    }
    m_status.state = state;
    m_status.detail = detail;
    if (state != State::InUse) m_status.width = m_status.height = m_status.fps = 0;
}

void VirtualCamera::RefreshInstallState() {
    const std::wstring registered = RegisteredDllPath();
    const std::wstring bundled = BundledDllPath();
    const bool update = !registered.empty() && GetFileAttributesW(bundled.c_str()) != INVALID_FILE_ATTRIBUTES &&
                        !SameFileContent(registered, bundled);
    DWORD persistent = 0;
    DWORD size = sizeof(persistent);
    RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, L"DauerhafteKamera", RRF_RT_REG_DWORD, nullptr, &persistent, &size);
    std::lock_guard lock(m_mutex);
    m_installed = !registered.empty();
    m_persistent = m_installed && persistent != 0;
    m_status.persistent = m_persistent;
    m_status.updateAvailable = update;
}

void VirtualCamera::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);  // Vorrang wie bei Video
    bool registered = false;
    bool attempted = false;
    for (;;) {
        if (m_register && (!attempted || m_retryRegistration.exchange(false))) {
            attempted = true;
            if (registered) Unregister();
            registered = Register();
        } else if (!m_register && !attempted) {
            attempted = true;
            registered = true;  // Testbetrieb: nur Bilder liefern
            SetState(State::Ready);
        }
        const HANDLE events[] = {m_stopEvent, m_frameEvent, m_wakeEvent};
        const DWORD result = WaitForMultipleObjects(3, events, FALSE, 500);
        if (result == WAIT_OBJECT_0) break;
        if (!registered) continue;
        const CameraFeeder::Consumer consumer = m_feeder.Feed();
        std::lock_guard lock(m_mutex);
        const State state = consumer.active ? State::InUse : State::Ready;
        if (m_status.state != state) LogLine("Virtuelle Kamera: %s", consumer.active ? "wird benutzt" : "wartet auf ein Programm");
        m_status.state = state;
        m_status.detail.clear();
        m_status.width = consumer.active ? static_cast<int>(consumer.width) : 0;
        m_status.height = consumer.active ? static_cast<int>(consumer.height) : 0;
        m_status.fps = consumer.active ? static_cast<int>(consumer.fps) : 0;
    }
    if (registered && m_register) Unregister();
    m_feeder.CloseShared();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

bool VirtualCamera::Register() {
    RefreshInstallState();
    const CreateVirtualCameraFn create = LoadCreateFunction();
    if (!create) {
        SetState(State::Unsupported, "Braucht Windows 11.");
        return false;
    }
    bool installed = false;
    {
        std::lock_guard lock(m_mutex);
        installed = m_installed;
    }
    if (!installed) {
        SetState(State::NotInstalled);
        return false;
    }
    bool persistent = false;
    {
        std::lock_guard lock(m_mutex);
        persistent = m_persistent;
    }
    if (persistent) {
        // Dauerhaft angemeldet (bei der Einrichtung): Windows kennt die Kamera schon.
        SetState(State::Ready);
        return true;
    }
    SetState(State::Starting);
    // Nur für diese Sitzung und diesen Benutzer: Die Kamera verschwindet, wenn KrakznCam endet,
    // und das Anmelden braucht keine Administratorrechte.
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                        MFVirtualCameraAccess_CurrentUser, VCam::kFriendlyName, VCam::kSourceClsidString, nullptr, 0,
                        &camera);
    if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
    if (FAILED(hr)) {
        char detail[96];
        snprintf(detail, sizeof(detail), "Windows hat die Kamera abgelehnt (Fehler 0x%08X).", static_cast<unsigned>(hr));
        SetState(State::Failed, detail);
        if (camera) camera->Shutdown();
        return false;
    }
    m_camera = camera;
    SetState(State::Ready);
    return true;
}

void VirtualCamera::Unregister() {
    if (!m_camera) return;
    m_camera->Remove();
    m_camera->Shutdown();
    m_camera.Reset();
    LogLine("Virtuelle Kamera abgemeldet");
}

void VirtualCamera::InstallAsync() {
    {
        std::lock_guard lock(m_mutex);
        if (m_status.installing) return;
        m_status.installing = true;
        m_status.installError.clear();
    }
    if (m_installThread.joinable()) m_installThread.join();
    m_installThread = std::thread([this] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = exe;
        info.lpParameters = L"--install-camera";
        info.nShow = SW_HIDE;
        std::string error;
        if (ShellExecuteExW(&info) && info.hProcess) {
            WaitForSingleObject(info.hProcess, 120000);
            DWORD code = 1;
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
            if (code != 0) {
                char text[96];
                snprintf(text, sizeof(text), "Einrichtung fehlgeschlagen (Fehler 0x%08X).", static_cast<unsigned>(code));
                error = text;
            }
        } else {
            error = GetLastError() == ERROR_CANCELLED ? "Abgebrochen: Windows braucht dafür deine Zustimmung."
                                                      : "Die Einrichtung ließ sich nicht starten.";
        }
        LogLine("Einrichtung der virtuellen Kamera: %s", error.empty() ? "erfolgreich" : error.c_str());
        {
            std::lock_guard lock(m_mutex);
            m_status.installing = false;
            m_status.installError = error;
        }
        m_retryRegistration = true;
        SetEvent(m_wakeEvent);
        CoUninitialize();
    });
}

int VirtualCamera::InstallElevated() {
    const std::wstring source = BundledDllPath();
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine("Einrichtung: %ls fehlt neben krakzncam.exe", VCam::kDllName);
        return static_cast<int>(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    }
    const std::wstring directory = InstallDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring target = directory + L"\\" + VCam::kDllName;
    if (!ReplaceFile(source, target)) return static_cast<int>(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
    CreateLogDirectory();
    const HRESULT hr = CallDllFunction(target, "DllRegisterServer");
    LogLine("Einrichtung: Kamera-DLL nach %ls kopiert, Anmeldung 0x%08X", target.c_str(), static_cast<unsigned>(hr));
    if (FAILED(hr)) return static_cast<int>(hr);
    RegisterPersistentCamera();
    return 0;
}

int VirtualCamera::UninstallElevated() {
    RemovePersistentCamera();
    const std::wstring target = InstallDirectory() + L"\\" + VCam::kDllName;
    HRESULT hr = S_OK;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) hr = CallDllFunction(target, "DllUnregisterServer");
    if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // gerade in Benutzung
    }
    RemoveDirectoryW(InstallDirectory().c_str());
    LogLine("Kamera entfernt (0x%08X)", static_cast<unsigned>(hr));
    return SUCCEEDED(hr) ? 0 : static_cast<int>(hr);
}
