#pragma once
#include "FrameStore.h"
#include "SetupCheck.h"
#include "StreamClient.h"
#include "VideoView.h"

#include <chrono>

struct ImFont;

struct AppFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
};

// Die Oberfläche: Vorschau links, Status, Signatur und Einrichtung rechts.
class App {
public:
    App(ID3D11Device* device, StreamClient& client, FrameStore& store, const AppFonts& fonts, float scale);

    // Fenster wurde aktiviert: Einrichtung neu prüfen (z. B. nach einer Installation).
    void OnActivated();
    // Holt ein neues Bild für die Vorschau. true = es gab ein neues.
    bool UpdateVideo();
    void Draw();

    VideoView& Video() { return m_video; }

private:
    void Recheck();
    void DrawVideo(const StreamStatus& status);
    void DrawConnection(const StreamStatus& status);
    void DrawSignature(const StreamStatus& status);
    void DrawSetup();
    void DrawVirtualCamera();
    void SectionTitle(const char* text);

    StreamClient& m_client;
    FrameStore& m_store;
    VideoView m_video;
    AppFonts m_fonts;
    float m_scale;
    SetupReport m_setup;
    std::chrono::steady_clock::time_point m_lastCheck;
};
