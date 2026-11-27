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

}  // namespace

VirtualCamera::VirtualCamera(ID3D11Device* device, FrameStore& store) : m_device(device), m_store(store) {
    m_device->GetImmediateContext(&m_context);
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
    ResetEvent(m_stopEvent);
    m_thread = std::thread([this] { Run(); });
}

void VirtualCamera::Stop() {
    if (!m_thread.joinable()) return;
    SetEvent(m_stopEvent);
    m_thread.join();
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
    std::lock_guard lock(m_mutex);
    m_installed = !registered.empty();
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
        if (registered) Feed();
    }
    if (registered && m_register) Unregister();
    CloseShared();
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

bool VirtualCamera::OpenShared() {
    if (m_header) return true;
    const ULONGLONG now = GetTickCount64();
    if (m_lastOpenAttempt != 0 && now - m_lastOpenAttempt < 1000) return false;
    m_lastOpenAttempt = now;
    // Die Kamera legt den Speicher an, sobald ein Programm sie öffnet.
    m_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, VCam::MappingName().c_str());
    if (!m_mapping) return false;
    m_header = static_cast<VCam::Header*>(MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VCam::kMappingSize));
    if (!m_header || m_header->magic != VCam::kMagic || m_header->version != VCam::kVersion) {
        LogLine("Gemeinsamer Speicher der Kamera passt nicht (Version)");
        CloseShared();
        return false;
    }
    // Ältere Kamera-DLLs kennen das Ereignis noch nicht; dann liefert sie nach eigener Uhr.
    m_frameReadyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, VCam::FrameEventName().c_str());
    LogLine("Gemeinsamer Speicher der Kamera geöffnet%s", m_frameReadyEvent ? "" : " (ohne Bild-Ereignis)");
    return true;
}

void VirtualCamera::CloseShared() {
    if (m_header) UnmapViewOfFile(m_header);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_frameReadyEvent) CloseHandle(m_frameReadyEvent);
    m_header = nullptr;
    m_mapping = nullptr;
    m_frameReadyEvent = nullptr;
}

bool VirtualCamera::ConfigureProcessor(ID3D11Texture2D* input, const FrameInfo& info, UINT width, UINT height) {
    m_inputView.Reset();
    m_outputView.Reset();
    m_processor.Reset();
    m_enumerator.Reset();
    m_output.Reset();
    m_staging[0].Reset();
    m_staging[1].Reset();
    m_stagingFull[0] = m_stagingFull[1] = false;
    m_configuredInput = nullptr;
    if (!m_videoDevice && (FAILED(m_device.As(&m_videoDevice)) || FAILED(m_context.As(&m_videoContext)))) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.InputWidth = info.codedWidth;
    content.InputHeight = info.codedHeight;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_enumerator);
    if (SUCCEEDED(hr)) hr = m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), 0, &m_processor);
    UINT support = 0;
    if (SUCCEEDED(hr)) m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support);
    if (FAILED(hr) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LogLine("Virtuelle Kamera: Grafikkarte kann nicht nach NV12 umrechnen (0x%08X, 0x%X)", static_cast<unsigned>(hr),
                support);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = m_device->CreateTexture2D(&desc, nullptr, &m_output);
    if (SUCCEEDED(hr)) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging[0]);
        if (SUCCEEDED(hr)) hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging[1]);
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessorOutputView(m_output.Get(), m_enumerator.Get(), &outputDesc, &m_outputView);
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessorInputView(input, m_enumerator.Get(), &inputDesc, &m_inputView);
    }
    if (FAILED(hr)) {
        LogLine("Virtuelle Kamera: Umrechnung nicht möglich (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }

    // Seitenverhältnis erhalten: notfalls schwarze Ränder statt Verzerrung.
    const float scale = std::min(static_cast<float>(width) / info.displayWidth, static_cast<float>(height) / info.displayHeight);
    const LONG drawnWidth = static_cast<LONG>(info.displayWidth * scale) & ~1L;
    const LONG drawnHeight = static_cast<LONG>(info.displayHeight * scale) & ~1L;
    const LONG left = (static_cast<LONG>(width) - drawnWidth) / 2 & ~1L;
    const LONG top = (static_cast<LONG>(height) - drawnHeight) / 2 & ~1L;
    RECT source{0, 0, static_cast<LONG>(info.displayWidth), static_cast<LONG>(info.displayHeight)};
    RECT destination{left, top, left + drawnWidth, top + drawnHeight};
    RECT target{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.Get(), 0, TRUE, &source);
    m_videoContext->VideoProcessorSetStreamDestRect(m_processor.Get(), 0, TRUE, &destination);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_processor.Get(), TRUE, &target);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {16.0f / 255.0f, 0.5f, 0.5f, 1.0f};
    m_videoContext->VideoProcessorSetOutputBackgroundColor(m_processor.Get(), TRUE, &black);
    // Ein- und Ausgang: BT.709 im Videobereich, wie vom iPhone.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE space{};
    space.YCbCr_Matrix = 1;
    space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &space);
    m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.Get(), &space);

    m_configuredInput = input;
    m_configuredInfo = info;
    m_outputWidth = width;
    m_outputHeight = height;
    LogLine("Virtuelle Kamera: rechne %ux%u nach %ux%u um", info.displayWidth, info.displayHeight, width, height);
    return true;
}

void VirtualCamera::Feed() {
    if (!OpenShared()) return;
    VCam::Header* header = m_header;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG consumer = static_cast<ULONGLONG>(header->consumerTick);
    const bool active = consumer != 0 && now - consumer < VCam::kConsumerTimeoutMs;
    UINT width = static_cast<UINT>(header->wantedWidth);
    UINT height = static_cast<UINT>(header->wantedHeight);
    UINT fps = static_cast<UINT>(header->wantedFps);
    if (width == 0 || height == 0 || width > VCam::kMaxWidth || height > VCam::kMaxHeight || width % 2 || height % 2) {
        width = 1920;
        height = 1080;
    }
    fps = std::clamp(fps, 1u, 120u);
    {
        std::lock_guard lock(m_mutex);
        const State state = active ? State::InUse : State::Ready;
        if (m_status.state != state) LogLine("Virtuelle Kamera: %s", active ? "wird benutzt" : "wartet auf ein Programm");
        m_status.state = state;
        m_status.detail.clear();
        m_status.width = active ? static_cast<int>(width) : 0;
        m_status.height = active ? static_cast<int>(height) : 0;
        m_status.fps = active ? static_cast<int>(fps) : 0;
    }
    // Ohne Zuschauer keine Arbeit; und nie schneller als die gewünschte Bildrate.
    if (!active || now < m_processorRetryAt) return;
    if (!m_frameReadyEvent && now - m_lastEventAttempt > 2000) {  // Kamera neu gestartet?
        m_lastEventAttempt = now;
        m_frameReadyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, VCam::FrameEventName().c_str());
    }
    // Nie schneller als die gewünschte Bildrate, aber mit Spielraum: Kommt ein Bild ein paar
    // Millisekunden zu früh, darf es nicht verloren gehen (genaue Uhr, nicht GetTickCount).
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    if (m_lastWriteTime != Clock::time_point{} && start - m_lastWriteTime < period * 3 / 4) return;
    for (auto& query : m_copyDone) {
        if (!query) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            m_device->CreateQuery(&desc, &query);
        }
    }
    const int current = m_stagingNext;
    const int previous = 1 - current;

    bool converted = false;
    m_store.WithLatest(m_lastCounter, [&](ID3D11Texture2D* input, const FrameInfo& info) {
        if (input != m_configuredInput || info.codedWidth != m_configuredInfo.codedWidth ||
            info.codedHeight != m_configuredInfo.codedHeight || info.displayWidth != m_configuredInfo.displayWidth ||
            info.displayHeight != m_configuredInfo.displayHeight || width != m_outputWidth || height != m_outputHeight) {
            if (!ConfigureProcessor(input, info, width, height)) {
                m_processorRetryAt = now + 5000;
                return;
            }
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = m_inputView.Get();
        if (FAILED(m_videoContext->VideoProcessorBlt(m_processor.Get(), m_outputView.Get(), 0, 1, &stream))) return;
        m_context->CopyResource(m_staging[current].Get(), m_output.Get());
        if (m_copyDone[current]) m_context->End(m_copyDone[current].Get());
        m_context->Flush();
        converted = true;
    });
    if (!converted) return;
    m_stagingFull[current] = true;
    m_stagingNext = previous;
    if (m_lastWriteTime != Clock::time_point{}) {
        m_statMaxGapMs = std::max(m_statMaxGapMs, std::chrono::duration<double, std::milli>(start - m_lastWriteTime).count());
    }
    m_lastWriteTime = start;
    // Das vorige Bild ist seit einem Bildabstand fertig: jetzt auslesen und weitergeben.
    // (Kostet ein Bild Verzögerung, dafür kommen die Bilder so gleichmäßig wie vom iPhone.)
    if (!m_stagingFull[previous]) return;
    while (m_copyDone[previous] &&
           m_context->GetData(m_copyDone[previous].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) {
        if (Clock::now() - start > std::chrono::milliseconds(200)) return;  // Grafikkarte hängt
        SwitchToThread();
    }
    m_stagingFull[previous] = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = m_context->Map(m_staging[previous].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return;

    InterlockedIncrement64(&header->sequence);  // ungerade: wird geschrieben
    header->frameWidth = static_cast<LONG>(width);
    header->frameHeight = static_cast<LONG>(height);
    uint8_t* target = VCam::FrameData(header);
    const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < height; ++y) memcpy(target + size_t(y) * width, source + size_t(y) * mapped.RowPitch, width);
    const uint8_t* sourceUv = source + size_t(mapped.RowPitch) * height;
    uint8_t* targetUv = target + size_t(width) * height;
    for (UINT y = 0; y < height / 2; ++y) {
        memcpy(targetUv + size_t(y) * width, sourceUv + size_t(y) * mapped.RowPitch, width);
    }
    m_context->Unmap(m_staging[previous].Get(), 0);
    InterlockedExchange64(&header->frameTick, static_cast<LONG64>(GetTickCount64()));
    InterlockedIncrement64(&header->frameNumber);
    InterlockedIncrement64(&header->sequence);  // gerade: fertig
    if (m_frameReadyEvent) SetEvent(m_frameReadyEvent);  // weckt die Kamera: neues Bild liegt bereit

    // Alle 10 s ins Protokoll: wie viele Bilder die Kamera bekommt und wie lange das Umrechnen dauert.
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    ++m_statFrames;
    m_statTotalMs += ms;
    m_statMaxMs = std::max(m_statMaxMs, ms);
    if (m_statStart == Clock::time_point{}) m_statStart = start;
    const double elapsed = std::chrono::duration<double>(Clock::now() - m_statStart).count();
    if (elapsed >= 10.0) {
        LogLine("Virtuelle Kamera: %.1f Bilder/s an %ux%u@%u, Umrechnen im Schnitt %.1f ms, höchstens %.1f ms, "
                "größter Abstand zwischen zwei Bildern %.1f ms",
                m_statFrames / elapsed, width, height, fps, m_statTotalMs / m_statFrames, m_statMaxMs, m_statMaxGapMs);
        m_statStart = Clock::now();
        m_statFrames = 0;
        m_statTotalMs = 0.0;
        m_statMaxMs = 0.0;
        m_statMaxGapMs = 0.0;
    }
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
    return SUCCEEDED(hr) ? 0 : static_cast<int>(hr);
}

int VirtualCamera::UninstallElevated() {
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
