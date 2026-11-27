#include "CameraFeeder.h"

#include "Log.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

CameraFeeder::CameraFeeder(ID3D11Device* device, FrameStore& store) : m_device(device), m_store(store) {
    m_device->GetImmediateContext(&m_context);
}

CameraFeeder::~CameraFeeder() {
    CloseShared();
}

bool CameraFeeder::OpenShared() {
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

void CameraFeeder::CloseShared() {
    if (m_header) UnmapViewOfFile(m_header);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_frameReadyEvent) CloseHandle(m_frameReadyEvent);
    m_header = nullptr;
    m_mapping = nullptr;
    m_frameReadyEvent = nullptr;
}

bool CameraFeeder::ConfigureProcessor(ID3D11Texture2D* input, const FrameInfo& info, UINT width, UINT height) {
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

CameraFeeder::Consumer CameraFeeder::Feed() {
    if (!OpenShared()) return {};
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
    const Consumer result{active, width, height, fps};
    // Ohne Zuschauer keine Arbeit; und nie schneller als die gewünschte Bildrate.
    if (!active || now < m_processorRetryAt) return result;
    if (!m_frameReadyEvent && now - m_lastEventAttempt > 2000) {  // Kamera neu gestartet?
        m_lastEventAttempt = now;
        m_frameReadyEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, VCam::FrameEventName().c_str());
    }
    // Nie schneller als die gewünschte Bildrate, aber mit Spielraum: Kommt ein Bild ein paar
    // Millisekunden zu früh, darf es nicht verloren gehen (genaue Uhr, nicht GetTickCount).
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    const auto period = std::chrono::microseconds(1'000'000 / fps);
    if (m_lastWriteTime != Clock::time_point{} && start - m_lastWriteTime < period * 3 / 4) return result;
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
    if (!converted) return result;
    m_stagingFull[current] = true;
    m_stagingNext = previous;
    if (m_lastWriteTime != Clock::time_point{}) {
        m_statMaxGapMs = std::max(m_statMaxGapMs, std::chrono::duration<double, std::milli>(start - m_lastWriteTime).count());
    }
    m_lastWriteTime = start;
    // Das vorige Bild ist seit einem Bildabstand fertig: jetzt auslesen und weitergeben.
    // (Kostet ein Bild Verzögerung, dafür kommen die Bilder so gleichmäßig wie vom iPhone.)
    if (!m_stagingFull[previous]) return result;
    while (m_copyDone[previous] &&
           m_context->GetData(m_copyDone[previous].Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) {
        if (Clock::now() - start > std::chrono::milliseconds(200)) return result;  // Grafikkarte hängt
        SwitchToThread();
    }
    m_stagingFull[previous] = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = m_context->Map(m_staging[previous].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return result;

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
    return result;
}

