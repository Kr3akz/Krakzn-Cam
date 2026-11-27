// krakzncam_vcam.dll: die virtuelle Kamera „KrakznCam“ für OBS, Discord, Teams usw.
//
// Windows 11 lädt diese DLL im Frame-Server-Dienst, sobald ein Programm die Kamera öffnet
// (angemeldet von krakzncam.exe mit MFCreateVirtualCamera). Sie besteht aus:
// - Activator (IMFActivate): das registrierte COM-Objekt, erzeugt die Media Source,
// - MediaSource (IMFMediaSourceEx): beschreibt den einen Videostrom und startet/stoppt ihn,
// - MediaStream (IMFMediaStream2): liefert die Bilder im Takt der gewählten Bildrate.
// Die Bilder kommen aus dem gemeinsamen Speicher (VCamShared.h); fehlt KrakznCam, zeigt die
// Kamera einen Platzhalter.

#include "Placeholder.h"
#include "VCamShared.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <sddl.h>
#include <share.h>
#include <wrl/client.h>
// ks.h zuletzt: Es braucht IUnknown und definiert GUID_NULL auf seine Art neu.
#include <avrt.h>
#include <winternl.h>  // NTSTATUS
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>  // IKsControl

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// --- Protokoll (C:\ProgramData\KrakznCam\vcam.log) ----------------------------------------

std::mutex g_logMutex;

void Log(const char* format, ...) {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramData%\\KrakznCam\\vcam.log", path, MAX_PATH)) return;
    std::lock_guard lock(g_logMutex);
    FILE* file = _wfsopen(path, L"ab", _SH_DENYNO);
    if (!file) return;
    fseek(file, 0, SEEK_END);
    if (ftell(file) > 512 * 1024) {  // klein halten
        fclose(file);
        file = _wfsopen(path, L"wb", _SH_DENYNO);
        if (!file) return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(file, "%02u.%02u. %02u:%02u:%02u.%03u  [%lu] ", now.wDay, now.wMonth, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, GetCurrentProcessId());
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

#define RETURN_IF_FAILED(expr)                                                                   \
    do {                                                                                          \
        const HRESULT hr_ = (expr);                                                               \
        if (FAILED(hr_)) {                                                                        \
            Log("Fehler 0x%08X in %s (Zeile %d)", static_cast<unsigned>(hr_), __func__, __LINE__); \
            return hr_;                                                                           \
        }                                                                                         \
    } while (0)

#define IMPLEMENT_REFCOUNT                                        \
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }    \
    STDMETHODIMP_(ULONG) Release() override {                     \
        const ULONG refs = --m_refs;                              \
        if (refs == 0) delete this;                               \
        return refs;                                              \
    }

// Angebotene Formate: NV12 in 1080p und 720p, je 30 und 60 Bilder pro Sekunde.
struct Format {
    UINT32 width;
    UINT32 height;
    UINT32 fps;
};
constexpr Format kFormats[] = {{1920, 1080, 30}, {1920, 1080, 60}, {1280, 720, 30}, {1280, 720, 60}};

HRESULT CreateMediaType(const Format& format, IMFMediaType** result) {
    ComPtr<IMFMediaType> type;
    RETURN_IF_FAILED(MFCreateMediaType(&type));
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, format.width, format.height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, format.fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(VCam::FrameBytes(format.width, format.height)));
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, format.width);
    type->SetUINT32(MF_MT_AVG_BITRATE, format.width * format.height * 12 * format.fps);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    *result = type.Detach();
    return S_OK;
}

// Merkmale, an denen der Frame Server einen normalen Farbkamera-Strom erkennt.
HRESULT SetStreamAttributes(IMFAttributes* attributes) {
    RETURN_IF_FAILED(attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}

// --- Gemeinsamer Speicher ------------------------------------------------------------------

class SharedFrames {
public:
    ~SharedFrames() { Close(); }

    // Kopiert das neueste Bild in passender Größe nach dst. false = keins da (Platzhalter zeigen).
    bool Read(uint32_t width, uint32_t height, uint32_t fps, BYTE* dst, LONG pitch) {
        if (!m_header && !Open()) return false;
        VCam::Header* header = m_header;
        InterlockedExchange(&header->wantedWidth, static_cast<LONG>(width));
        InterlockedExchange(&header->wantedHeight, static_cast<LONG>(height));
        InterlockedExchange(&header->wantedFps, static_cast<LONG>(fps));
        const ULONGLONG now = GetTickCount64();
        InterlockedExchange64(&header->consumerTick, static_cast<LONG64>(now));

        const LONG64 sequence = InterlockedOr64(&header->sequence, 0);
        if ((sequence & 1) == 0 && sequence != m_cacheSequence) {
            const LONG frameWidth = header->frameWidth;
            const LONG frameHeight = header->frameHeight;
            if (frameWidth > 0 && frameHeight > 0 && frameWidth <= static_cast<LONG>(VCam::kMaxWidth) &&
                frameHeight <= static_cast<LONG>(VCam::kMaxHeight) && frameWidth % 2 == 0 && frameHeight % 2 == 0) {
                const size_t bytes = VCam::FrameBytes(frameWidth, frameHeight);
                m_scratch.resize(bytes);
                memcpy(m_scratch.data(), VCam::FrameData(header), bytes);
                MemoryBarrier();
                if (InterlockedOr64(&header->sequence, 0) == sequence) {
                    m_cache.swap(m_scratch);
                    m_cacheWidth = static_cast<uint32_t>(frameWidth);
                    m_cacheHeight = static_cast<uint32_t>(frameHeight);
                    m_cacheSequence = sequence;
                } else {
                    ++collisions;  // KrakznCam schrieb gerade: altes Bild behalten
                }
            }
        }
        const ULONGLONG frameTick = static_cast<ULONGLONG>(header->frameTick);
        if (m_cacheSequence < 0 || now - frameTick > VCam::kFrameTimeoutMs) return false;
        if (m_cacheWidth != width || m_cacheHeight != height) return false;
        CopyPlanes(m_cache.data(), width, height, dst, pitch);
        return true;
    }

    // Ereignis „neues Bild“ von KrakznCam (null, solange der gemeinsame Speicher fehlt).
    HANDLE FrameEvent() {
        if (!m_header) Open();
        return m_frameEvent;
    }

    // Liegt ein Bild bereit, das noch nicht ausgeliefert wurde?
    bool HasNewFrame() const {
        if (!m_header) return false;
        const LONG64 sequence = InterlockedOr64(&m_header->sequence, 0);
        return (sequence & 1) == 0 && sequence != m_cacheSequence && sequence != 0;
    }

    static void CopyPlanes(const uint8_t* source, uint32_t width, uint32_t height, BYTE* dst, LONG pitch) {
        for (uint32_t y = 0; y < height; ++y) memcpy(dst + LONG_PTR(y) * pitch, source + size_t(y) * width, width);
        const uint8_t* sourceUv = source + size_t(width) * height;
        BYTE* dstUv = dst + LONG_PTR(height) * pitch;
        for (uint32_t y = 0; y < height / 2; ++y) {
            memcpy(dstUv + LONG_PTR(y) * pitch, sourceUv + size_t(y) * width, width);
        }
    }

private:
    bool Open() {
        const ULONGLONG now = GetTickCount64();
        if (m_lastAttempt != 0 && now - m_lastAttempt < 1000) return false;
        m_lastAttempt = now;
        // Der Dienst legt den Speicher an; angemeldete Benutzer dürfen lesen und schreiben.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGW;;;AU)", SDDL_REVISION_1, &descriptor, nullptr);
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        const ULARGE_INTEGER size{.QuadPart = VCam::kMappingSize};
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, descriptor ? &security : nullptr, PAGE_READWRITE,
                                       size.HighPart, size.LowPart, VCam::MappingName().c_str());
        const DWORD error = GetLastError();
        if (!m_frameEvent) {
            m_frameEvent = CreateEventW(descriptor ? &security : nullptr, FALSE, FALSE, VCam::FrameEventName().c_str());
            if (!m_frameEvent) m_frameEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, VCam::FrameEventName().c_str());
        }
        if (descriptor) LocalFree(descriptor);
        bool created = m_mapping && error != ERROR_ALREADY_EXISTS;
        if (!m_mapping) {
            m_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, VCam::MappingName().c_str());
            created = false;
        }
        if (!m_mapping) {
            if (!m_loggedFailure) Log("Gemeinsamer Speicher nicht verfügbar (Fehler %lu)", error);
            m_loggedFailure = true;
            return false;
        }
        m_header = static_cast<VCam::Header*>(
            MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VCam::kMappingSize));
        if (!m_header) {
            Log("Gemeinsamer Speicher lässt sich nicht einblenden (Fehler %lu)", GetLastError());
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }
        if (created) {
            m_header->version = VCam::kVersion;
            m_header->magic = VCam::kMagic;
        }
        Log("Gemeinsamer Speicher %s", created ? "angelegt" : "geöffnet");
        return true;
    }

    void Close() {
        if (m_header) UnmapViewOfFile(m_header);
        if (m_mapping) CloseHandle(m_mapping);
        if (m_frameEvent) CloseHandle(m_frameEvent);
        m_header = nullptr;
        m_mapping = nullptr;
        m_frameEvent = nullptr;
    }

    HANDLE m_mapping = nullptr;
    HANDLE m_frameEvent = nullptr;
    VCam::Header* m_header = nullptr;
    ULONGLONG m_lastAttempt = 0;
    bool m_loggedFailure = false;
    std::vector<uint8_t> m_cache;
    std::vector<uint8_t> m_scratch;
    LONG64 m_cacheSequence = -1;
    uint32_t m_cacheWidth = 0;
    uint32_t m_cacheHeight = 0;

public:
    uint32_t collisions = 0;  // Statistik: Lesen während des Schreibens
};

// --- Videostrom ----------------------------------------------------------------------------

class MediaStream final : public IMFMediaStream2, public IKsControl {
public:
    MediaStream() { ++g_objects; }

    HRESULT Initialize(IMFMediaSource* source) {
        m_source = source;
        RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 8));
        RETURN_IF_FAILED(SetStreamAttributes(m_attributes.Get()));
        RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));

        std::vector<ComPtr<IMFMediaType>> types;
        std::vector<IMFMediaType*> raw;
        for (const Format& format : kFormats) {
            ComPtr<IMFMediaType> type;
            RETURN_IF_FAILED(CreateMediaType(format, &type));
            raw.push_back(type.Get());
            types.push_back(type);
        }
        RETURN_IF_FAILED(MFCreateStreamDescriptor(0, static_cast<DWORD>(raw.size()), raw.data(), &m_descriptor));
        ComPtr<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->SetCurrentMediaType(raw[0]));
        RETURN_IF_FAILED(SetStreamAttributes(m_descriptor.Get()));
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return S_OK;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
            riid == __uuidof(IMFMediaStream2)) {
            *ppv = static_cast<IMFMediaStream2*>(this);
        } else if (riid == __uuidof(IKsControl)) {
            *ppv = static_cast<IKsControl*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IMPLEMENT_REFCOUNT

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->EndGetEvent(result, event) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->GetEvent(flags, event) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->QueueEventParamVar(type, extended, status, value) : MF_E_SHUTDOWN;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override {
        if (!source) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown || !m_source) return MF_E_SHUTDOWN;
        *source = m_source;
        m_source->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override {
        if (!descriptor) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_descriptor.CopyTo(descriptor);
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            if (!m_started || m_state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
            m_tokens.emplace_back(token);
        }
        SetEvent(m_requestEvent);
        return S_OK;
    }

    // IMFMediaStream2: wird vom Frame Server zum Ein- und Ausschalten des Stroms benutzt.
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        switch (state) {
        case MF_STREAM_STATE_RUNNING:
        case MF_STREAM_STATE_STOPPED: m_state = state; break;
        case MF_STREAM_STATE_PAUSED: return S_OK;  // nicht unterstützt
        default: return MF_E_INVALID_STATE_TRANSITION;
        }
        if (state == MF_STREAM_STATE_STOPPED) m_tokens.clear();
        Log("Stromzustand: %s", state == MF_STREAM_STATE_RUNNING ? "läuft" : "gestoppt");
        return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override {
        if (!state) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *state = m_state;
        return S_OK;
    }

    // IKsControl: keine Treiber-Eigenschaften.
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

    IMFAttributes* Attributes() const { return m_attributes.Get(); }

    // Von der Media Source: Strom mit dem gewählten Format starten.
    HRESULT Start(IMFMediaType* type) {
        StopWorker();
        UINT32 width = 0, height = 0, fpsNumerator = 0, fpsDenominator = 0;
        RETURN_IF_FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
        RETURN_IF_FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fpsNumerator, &fpsDenominator));
        ComPtr<IMFVideoSampleAllocatorEx> allocator;
        if (SUCCEEDED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator)))) {
            if (FAILED(allocator->InitializeSampleAllocator(8, type))) allocator.Reset();
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            m_width = width;
            m_height = height;
            m_fps = fpsDenominator ? std::max(1u, fpsNumerator / fpsDenominator) : 30u;
            m_allocator = allocator;
            m_tokens.clear();
            m_started = true;
            m_state = MF_STREAM_STATE_RUNNING;
        }
        ResetEvent(m_stopEvent);
        m_worker = std::thread([this] { Worker(); });
        Log("Strom startet: %ux%u, %u FPS%s", width, height, m_fps, allocator ? "" : " (ohne Sample-Allocator)");
        RETURN_IF_FAILED(QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr));
        return S_OK;
    }

    HRESULT Stop() {
        StopWorker();
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            m_started = false;
            m_tokens.clear();
            if (m_allocator) m_allocator->UninitializeSampleAllocator();
            m_allocator.Reset();
        }
        Log("Strom gestoppt");
        return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    void Shutdown() {
        StopWorker();
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return;
        m_shutdown = true;
        m_source = nullptr;
        m_tokens.clear();
        if (m_allocator) m_allocator->UninitializeSampleAllocator();
        m_allocator.Reset();
        if (m_queue) m_queue->Shutdown();
    }

private:
    ~MediaStream() {
        StopWorker();
        if (m_stopEvent) CloseHandle(m_stopEvent);
        if (m_requestEvent) CloseHandle(m_requestEvent);
        --g_objects;
    }

    ComPtr<IMFMediaEventQueue> Queue() {
        std::lock_guard lock(m_mutex);
        return m_shutdown ? nullptr : m_queue;
    }

    void StopWorker() {
        if (!m_worker.joinable()) return;
        SetEvent(m_stopEvent);
        m_worker.join();
    }

    // Liefert ein Bild, sobald KrakznCam ein neues bereitlegt (höchstens mit der gewünschten
    // Bildrate). Kommt nichts Neues, wird das letzte Bild bzw. der Platzhalter wiederholt.
    // Früher lief hier eine eigene Uhr; gegen die 60-Hz-Uhr des iPhones gab das doppelte und
    // fehlende Bilder, also Ruckeln.
    void Worker() {
        using Clock = std::chrono::steady_clock;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Vorrang wie bei Video-Aufnahmen, damit es unter Last (Spiel, OBS) nicht hakt.
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
        // Hochauflösender Wecker: normale Wartezeiten sind unter Windows oft auf 16 ms gerundet.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        uint32_t fps = 30;
        {
            std::lock_guard lock(m_mutex);
            fps = m_fps;
        }
        const Clock::duration period = std::chrono::microseconds(1'000'000 / fps);
        Clock::time_point lastDelivery{};
        Clock::time_point statStart{};
        uint32_t statFresh = 0;
        uint32_t statRepeat = 0;
        for (;;) {
            bool pending = false;
            {
                std::lock_guard lock(m_mutex);
                pending = !m_tokens.empty() && m_state == MF_STREAM_STATE_RUNNING;
            }
            const HANDLE frameEvent = m_frames.FrameEvent();
            Clock::duration wait = std::chrono::seconds(1);
            if (pending) {
                const Clock::time_point now = Clock::now();
                const Clock::duration since = now - lastDelivery;
                const bool fresh = m_frames.HasNewFrame();
                // Neues Bild: sofort, aber nicht schneller als die Bildrate (mit Spielraum fürs Zittern).
                // Nichts Neues: ohne KrakznCam im eigenen Takt (Platzhalter), sonst nach zwei Bildern Pause wiederholen.
                const Clock::duration earliest = period * 3 / 4;
                const Clock::duration repeat = frameEvent ? period * 2 : period;
                const Clock::duration needed = fresh ? earliest : repeat;
                if (lastDelivery == Clock::time_point{} || since >= needed) {
                    Deliver();
                    lastDelivery = now;
                    // Statistik fürs Protokoll (alle 10 s): neue Bilder, Wiederholungen, Lese-Kollisionen.
                    if (fresh) ++statFresh;
                    else ++statRepeat;
                    if (statStart == Clock::time_point{}) statStart = now;
                    if (now - statStart >= std::chrono::seconds(10)) {
                        Log("Ausgeliefert in 10 s: %u neue Bilder, %u Wiederholungen, %u Kollisionen (%u FPS gewünscht)",
                            statFresh, statRepeat, m_frames.collisions, fps);
                        statFresh = statRepeat = m_frames.collisions = 0;
                        statStart = now;
                    }
                    continue;
                }
                wait = needed - since;
            }
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(wait).count());
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE events[] = {m_stopEvent, m_requestEvent, timer, frameEvent};
            const DWORD result = WaitForMultipleObjects(frameEvent ? 4 : 3, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) break;
        }
        if (timer) {
            CancelWaitableTimer(timer);
            CloseHandle(timer);
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
    }

    void Deliver() {
        ComPtr<IUnknown> token;
        ComPtr<IMFVideoSampleAllocatorEx> allocator;
        ComPtr<IMFMediaEventQueue> queue;
        uint32_t width = 0, height = 0, fps = 30;
        {
            std::lock_guard lock(m_mutex);
            if (m_tokens.empty() || m_shutdown) return;
            token = m_tokens.front();
            m_tokens.pop_front();
            allocator = m_allocator;
            queue = m_queue;
            width = m_width;
            height = m_height;
            fps = m_fps;
        }
        ComPtr<IMFSample> sample;
        HRESULT hr = E_FAIL;
        if (allocator) {
            hr = allocator->AllocateSample(&sample);
            if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {  // alle Bilder noch unterwegs: dieses auslassen
                std::lock_guard lock(m_mutex);
                m_tokens.push_front(token);
                return;
            }
        }
        if (FAILED(hr)) {
            ComPtr<IMFMediaBuffer> buffer;
            hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = MFCreate2DMediaBuffer(width, height, MFVideoFormat_NV12.Data1, FALSE, &buffer);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
            if (FAILED(hr)) {
                Log("Bildpuffer fehlt (0x%08X)", static_cast<unsigned>(hr));
                return;
            }
        }
        if (FAILED(Fill(sample.Get(), width, height, fps))) return;
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(10'000'000LL / fps);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());
        if (queue) queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
        if (++m_delivered % 1800 == 0) Log("%llu Bilder geliefert", static_cast<unsigned long long>(m_delivered));
    }

    HRESULT Fill(IMFSample* sample, uint32_t width, uint32_t height, uint32_t fps) {
        ComPtr<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));
        ComPtr<IMF2DBuffer2> buffer2d;
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        BYTE* start = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&buffer2d))) {
            RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length));
        } else {
            DWORD maxLength = 0;
            RETURN_IF_FAILED(buffer->Lock(&scanline0, &maxLength, nullptr));
            pitch = static_cast<LONG>(width);
        }
        if (!m_frames.Read(width, height, fps, scanline0, pitch)) {
            SharedFrames::CopyPlanes(m_placeholder.Get(width, height).data(), width, height, scanline0, pitch);
        }
        if (buffer2d) {
            buffer2d->Unlock2D();
            DWORD contiguous = 0;
            if (SUCCEEDED(buffer2d->GetContiguousLength(&contiguous))) buffer->SetCurrentLength(contiguous);
        } else {
            buffer->Unlock();
            buffer->SetCurrentLength(static_cast<DWORD>(VCam::FrameBytes(width, height)));
        }
        return S_OK;
    }

    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex;
    IMFMediaSource* m_source = nullptr;  // ohne Referenz: die Quelle besitzt den Strom
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<IMFVideoSampleAllocatorEx> m_allocator;
    std::deque<ComPtr<IUnknown>> m_tokens;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_started = false;
    bool m_shutdown = false;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint32_t m_fps = 30;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_requestEvent = nullptr;
    std::thread m_worker;
    uint64_t m_delivered = 0;
    // Nur im Arbeits-Thread benutzt.
    SharedFrames m_frames;
    Placeholder m_placeholder;
};

// --- Media Source --------------------------------------------------------------------------

class MediaSource final : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
public:
    MediaSource() { ++g_objects; }

    HRESULT Initialize(IMFAttributes* activatorAttributes) {
        RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
        if (activatorAttributes) activatorAttributes->CopyAllItems(m_attributes.Get());
        RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));
        m_stream.Attach(new (std::nothrow) MediaStream());
        if (!m_stream) return E_OUTOFMEMORY;
        RETURN_IF_FAILED(m_stream->Initialize(this));
        ComPtr<IMFStreamDescriptor> descriptor;
        RETURN_IF_FAILED(m_stream->GetStreamDescriptor(&descriptor));
        IMFStreamDescriptor* descriptors[] = {descriptor.Get()};
        RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, &m_presentation));
        RETURN_IF_FAILED(m_presentation->SelectStream(0));
        Log("Virtuelle Kamera geladen");
        return S_OK;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
            riid == __uuidof(IMFMediaSourceEx)) {
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        } else if (riid == __uuidof(IMFGetService)) {
            *ppv = static_cast<IMFGetService*>(this);
        } else if (riid == __uuidof(IKsControl)) {
            *ppv = static_cast<IKsControl*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IMPLEMENT_REFCOUNT

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->EndGetEvent(result, event) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->GetEvent(flags, event) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override {
        ComPtr<IMFMediaEventQueue> queue = Queue();
        return queue ? queue->QueueEventParamVar(type, extended, status, value) : MF_E_SHUTDOWN;
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override {
        if (!characteristics) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        *characteristics = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override {
        if (!descriptor) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_presentation->Clone(descriptor);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override {
        if (!descriptor || !startPosition) return E_INVALIDARG;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        ComPtr<MediaStream> stream;
        ComPtr<IMFMediaEventQueue> queue;
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            stream = m_stream;
            queue = m_queue;
        }
        DWORD count = 0;
        RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
        for (DWORD i = 0; i < count; ++i) {
            BOOL selected = FALSE;
            ComPtr<IMFStreamDescriptor> streamDescriptor;
            RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(i, &selected, &streamDescriptor));
            DWORD id = 0;
            RETURN_IF_FAILED(streamDescriptor->GetStreamIdentifier(&id));
            if (id != 0) continue;
            if (selected) {
                ComPtr<IMFMediaTypeHandler> handler;
                ComPtr<IMFMediaType> type;
                RETURN_IF_FAILED(streamDescriptor->GetMediaTypeHandler(&handler));
                RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
                RETURN_IF_FAILED(queue->QueueEventParamUnk(m_streamAnnounced ? MEUpdatedStream : MENewStream, GUID_NULL,
                                                           S_OK, static_cast<IMFMediaStream2*>(stream.Get())));
                m_streamAnnounced = true;
                RETURN_IF_FAILED(stream->Start(type.Get()));
            } else {
                stream->Stop();
            }
        }
        return queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, startPosition);
    }
    STDMETHODIMP Stop() override {
        ComPtr<MediaStream> stream;
        ComPtr<IMFMediaEventQueue> queue;
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            stream = m_stream;
            queue = m_queue;
        }
        stream->Stop();
        return queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        ComPtr<MediaStream> stream;
        {
            std::lock_guard lock(m_mutex);
            if (m_shutdown) return MF_E_SHUTDOWN;
            m_shutdown = true;
            stream = m_stream;
            m_stream.Reset();
            if (m_queue) m_queue->Shutdown();
        }
        if (stream) stream->Shutdown();
        Log("Virtuelle Kamera beendet");
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override {
        if (!attributes) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        return m_attributes.CopyTo(attributes);
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** attributes) override {
        if (!attributes) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (m_shutdown) return MF_E_SHUTDOWN;
        if (id != 0 || !m_stream) return MF_E_INVALIDSTREAMNUMBER;
        *attributes = m_stream->Attributes();
        (*attributes)->AddRef();
        return S_OK;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }  // Bilder liegen im Arbeitsspeicher

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl
    STDMETHODIMP_(NTSTATUS) KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP_(NTSTATUS) KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    STDMETHODIMP_(NTSTATUS) KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

private:
    ~MediaSource() {
        if (m_stream) m_stream->Shutdown();
        --g_objects;
    }

    ComPtr<IMFMediaEventQueue> Queue() {
        std::lock_guard lock(m_mutex);
        return m_shutdown ? nullptr : m_queue;
    }

    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaEventQueue> m_queue;
    ComPtr<IMFPresentationDescriptor> m_presentation;
    ComPtr<MediaStream> m_stream;
    bool m_streamAnnounced = false;
    bool m_shutdown = false;
};

// --- Activator (das registrierte COM-Objekt) -------------------------------------------------

class Activator final : public IMFActivate {
public:
    Activator() { ++g_objects; }

    HRESULT Initialize() { return MFCreateAttributes(&m_attributes, 4); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IMPLEMENT_REFCOUNT

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        std::lock_guard lock(m_mutex);
        if (!m_source) {
            ComPtr<MediaSource> source;
            source.Attach(new (std::nothrow) MediaSource());
            if (!source) return E_OUTOFMEMORY;
            RETURN_IF_FAILED(source->Initialize(m_attributes.Get()));
            m_source = source;
        }
        return m_source->QueryInterface(riid, ppv);
    }
    STDMETHODIMP ShutdownObject() override {
        ComPtr<MediaSource> source;
        {
            std::lock_guard lock(m_mutex);
            source.Swap(m_source);
        }
        if (source) source->Shutdown();
        return S_OK;
    }
    STDMETHODIMP DetachObject() override {
        std::lock_guard lock(m_mutex);
        m_source.Reset();
        return S_OK;
    }

    // IMFAttributes: alles an den eigenen Speicher weiterreichen.
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return m_attributes->GetItem(key, value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override { return m_attributes->GetItemType(key, type); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
        return m_attributes->CompareItem(key, value, result);
    }
    STDMETHODIMP Compare(IMFAttributes* other, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override {
        return m_attributes->Compare(other, type, result);
    }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return m_attributes->GetUINT32(key, value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return m_attributes->GetUINT64(key, value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return m_attributes->GetDouble(key, value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return m_attributes->GetGUID(key, value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override {
        return m_attributes->GetStringLength(key, length);
    }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override {
        return m_attributes->GetString(key, value, size, length);
    }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override {
        return m_attributes->GetAllocatedString(key, value, length);
    }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return m_attributes->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* written) override {
        return m_attributes->GetBlob(key, buffer, size, written);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override {
        return m_attributes->GetAllocatedBlob(key, buffer, size);
    }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override {
        return m_attributes->GetUnknown(key, riid, ppv);
    }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return m_attributes->SetItem(key, value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return m_attributes->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return m_attributes->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return m_attributes->SetUINT32(key, value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return m_attributes->SetUINT64(key, value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return m_attributes->SetDouble(key, value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return m_attributes->SetGUID(key, value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return m_attributes->SetString(key, value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override {
        return m_attributes->SetBlob(key, buffer, size);
    }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return m_attributes->SetUnknown(key, value); }
    STDMETHODIMP LockStore() override { return m_attributes->LockStore(); }
    STDMETHODIMP UnlockStore() override { return m_attributes->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return m_attributes->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override {
        return m_attributes->GetItemByIndex(index, key, value);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes* destination) override { return m_attributes->CopyAllItems(destination); }

private:
    ~Activator() { --g_objects; }

    std::atomic<ULONG> m_refs{1};
    std::mutex m_mutex;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<MediaSource> m_source;
};

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // statisches Objekt
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<Activator> activator;
        activator.Attach(new (std::nothrow) Activator());
        if (!activator) return E_OUTOFMEMORY;
        RETURN_IF_FAILED(activator->Initialize());
        return activator->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

std::wstring ClsidKey() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + VCam::kSourceClsidString;
}

}  // namespace

// --- DLL-Einstiegspunkte ---------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != VCam::kSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE;
}

// Meldet die Kamera-Quelle für alle Benutzer an (HKLM, braucht Administratorrechte).
// Der Frame-Server-Dienst sieht nur diese Anmeldung, nicht die des Benutzers.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (ClsidKey() + L"\\InprocServer32").c_str(), 0, nullptr, 0,
                                     KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path), (length + 1) * sizeof(wchar_t));
    static const wchar_t kModel[] = L"Both";
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(kModel), sizeof(kModel));
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    static const wchar_t kName[] = L"KrakznCam Virtual Camera";
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, ClsidKey().c_str(), nullptr, REG_SZ, kName, sizeof(kName));
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKey().c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
