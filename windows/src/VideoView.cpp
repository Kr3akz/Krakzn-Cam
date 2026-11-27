#include "VideoView.h"

#include "Log.h"

using Microsoft::WRL::ComPtr;

bool VideoView::Init(ID3D11Device* device) {
    m_device = device;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (FAILED(m_device.As(&m_videoDevice)) || FAILED(context.As(&m_videoContext))) {
        LogLine("Grafikkarte bietet keinen Video-Prozessor an");
        return false;
    }
    return true;
}

VideoView::ConfigureResult VideoView::Configure(ID3D11Texture2D* input, const FrameInfo& info) {
    m_inputView.Reset();
    m_outputView.Reset();
    m_processor.Reset();
    m_enumerator.Reset();
    m_srv.Reset();
    m_output.Reset();
    m_configuredInput = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.InputWidth = info.codedWidth;
    content.InputHeight = info.codedHeight;
    content.OutputWidth = info.displayWidth;
    content.OutputHeight = info.displayHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_enumerator);
    if (SUCCEEDED(hr)) hr = m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), 0, &m_processor);
    if (FAILED(hr)) {
        LogLine("Video-Prozessor konnte nicht erstellt werden (0x%08X)", static_cast<unsigned>(hr));
        return ConfigureResult::Failed;
    }
    UINT nv12Support = 0;
    UINT bgraSupport = 0;
    m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12Support);
    m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &bgraSupport);
    LogLine("Video-Prozessor: NV12-Unterstützung 0x%X, BGRA-Unterstützung 0x%X", nv12Support, bgraSupport);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info.displayWidth;
    desc.Height = info.displayHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&desc, nullptr, &m_output);
    if (SUCCEEDED(hr)) hr = m_device->CreateShaderResourceView(m_output.Get(), nullptr, &m_srv);
    if (FAILED(hr)) {
        LogLine("Vorschau-Textur konnte nicht erstellt werden (0x%08X)", static_cast<unsigned>(hr));
        return ConfigureResult::Failed;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = m_videoDevice->CreateVideoProcessorOutputView(m_output.Get(), m_enumerator.Get(), &outputDesc, &m_outputView);
    if (FAILED(hr)) {
        LogLine("Ausgabe-Ansicht des Video-Prozessors fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
        return ConfigureResult::Failed;
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = m_videoDevice->CreateVideoProcessorInputView(input, m_enumerator.Get(), &inputDesc, &m_inputView);
    if (FAILED(hr)) {
        D3D11_TEXTURE2D_DESC inputTexture{};
        input->GetDesc(&inputTexture);
        LogLine("Eingabe-Ansicht abgelehnt (0x%08X) für Textur %ux%u, Bind 0x%X", static_cast<unsigned>(hr),
                inputTexture.Width, inputTexture.Height, inputTexture.BindFlags);
        return ConfigureResult::InputRejected;
    }

    RECT visible{0, 0, static_cast<LONG>(info.displayWidth), static_cast<LONG>(info.displayHeight)};
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.Get(), 0, TRUE, &visible);
    m_videoContext->VideoProcessorSetStreamDestRect(m_processor.Get(), 0, TRUE, &visible);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_processor.Get(), TRUE, &visible);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);

    // Das iPhone liefert BT.709 im Videobereich (16-235).
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.YCbCr_Matrix = 1;
    inputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &inputSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    outputSpace.RGB_Range = 0;  // volle RGB-Skala
    outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.Get(), &outputSpace);

    m_configuredInput = input;
    m_codedWidth = info.codedWidth;
    m_codedHeight = info.codedHeight;
    m_width = info.displayWidth;
    m_height = info.displayHeight;
    LogLine("Vorschau eingerichtet: %ux%u", m_width, m_height);
    ConfigureAmbient(input, info);
    return ConfigureResult::Ok;
}

void VideoView::ConfigureAmbient(ID3D11Texture2D* input, const FrameInfo& info) {
    // Zweiter, winziger Ausgang des Video-Prozessors. Groß gezogen wirkt er wie ein
    // Weichzeichner – ohne eigenen Shader und praktisch ohne Rechenaufwand.
    constexpr UINT kWidth = 32;
    constexpr UINT kHeight = 18;
    m_ambientInputView.Reset();
    m_ambientOutputView.Reset();
    m_ambientProcessor.Reset();
    m_ambientEnumerator.Reset();
    m_ambientSrv.Reset();
    m_ambient.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.OutputFrameRate = {60, 1};
    content.InputWidth = info.codedWidth;
    content.InputHeight = info.codedHeight;
    content.OutputWidth = kWidth;
    content.OutputHeight = kHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;

    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_ambientEnumerator);
    if (SUCCEEDED(hr)) hr = m_videoDevice->CreateVideoProcessor(m_ambientEnumerator.Get(), 0, &m_ambientProcessor);
    if (SUCCEEDED(hr)) hr = m_device->CreateTexture2D(&desc, nullptr, &m_ambient);
    if (SUCCEEDED(hr)) hr = m_device->CreateShaderResourceView(m_ambient.Get(), nullptr, &m_ambientSrv);
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessorOutputView(m_ambient.Get(), m_ambientEnumerator.Get(), &outputDesc,
                                                           &m_ambientOutputView);
    }
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessorInputView(input, m_ambientEnumerator.Get(), &inputDesc,
                                                          &m_ambientInputView);
    }
    if (FAILED(hr)) {
        LogLine("Hintergrund-Schein nicht verfügbar (0x%08X), nutze das Vorschaubild", static_cast<unsigned>(hr));
        m_ambientProcessor.Reset();
        m_ambientSrv.Reset();
        return;
    }

    RECT visible{0, 0, static_cast<LONG>(info.displayWidth), static_cast<LONG>(info.displayHeight)};
    RECT target{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_ambientProcessor.Get(), 0,
                                                       D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamSourceRect(m_ambientProcessor.Get(), 0, TRUE, &visible);
    m_videoContext->VideoProcessorSetStreamDestRect(m_ambientProcessor.Get(), 0, TRUE, &target);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_ambientProcessor.Get(), TRUE, &target);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_ambientProcessor.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.YCbCr_Matrix = 1;
    inputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoContext->VideoProcessorSetStreamColorSpace(m_ambientProcessor.Get(), 0, &inputSpace);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    m_videoContext->VideoProcessorSetOutputColorSpace(m_ambientProcessor.Get(), &outputSpace);
}

bool VideoView::Update(FrameStore& store) {
    if (!m_videoContext || m_failed) return false;
    bool converted = false;
    bool inputRejected = false;
    store.WithLatest(m_lastCounter, [&](ID3D11Texture2D* input, const FrameInfo& info) {
        if (input != m_configuredInput || info.codedWidth != m_codedWidth || info.codedHeight != m_codedHeight ||
            info.displayWidth != m_width || info.displayHeight != m_height) {
            const ConfigureResult result = Configure(input, info);
            if (result == ConfigureResult::InputRejected) {
                inputRejected = true;
                return;
            }
            if (result != ConfigureResult::Ok) {
                m_failed = true;
                return;
            }
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = m_inputView.Get();
        HRESULT hr = m_videoContext->VideoProcessorBlt(m_processor.Get(), m_outputView.Get(), 0, 1, &stream);
        if (FAILED(hr)) {
            LogLine("VideoProcessorBlt fehlgeschlagen (0x%08X)", static_cast<unsigned>(hr));
            return;
        }
        converted = true;
        m_hasFrame = true;
        if (m_ambientProcessor) {
            D3D11_VIDEO_PROCESSOR_STREAM ambient{};
            ambient.Enable = TRUE;
            ambient.pInputSurface = m_ambientInputView.Get();
            m_videoContext->VideoProcessorBlt(m_ambientProcessor.Get(), m_ambientOutputView.Get(), 0, 1, &ambient);
        }
    });
    // Außerhalb der Sperre: Bildablage mit anderer Textur-Variante neu anlegen lassen.
    if (inputRejected && !store.TryNextTextureVariant()) {
        LogLine("Keine Textur-Variante passt zum Video-Prozessor dieser Grafikkarte");
        m_failed = true;
    }
    return converted;
}
