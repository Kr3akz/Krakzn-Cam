#pragma once
#include "FrameStore.h"
#include "SetupCheck.h"
#include "StreamClient.h"
#include "VideoView.h"

#include "imgui.h"

#include <chrono>

// Die Oberfläche im Stil von Eidolon: Kopfzeile mit Reitern, die Seite „Kamera“ mit Vorschau,
// Titel und Kennzahlen, und die Seite „Einrichtung“ als Karte mit einer Liste.
class App {
public:
    enum class Page { Camera, Setup };

    // dpiScale: Skalierung des Bildschirms (1,0 = 100 %). Die Oberfläche wächst zusätzlich mit dem Fenster.
    App(ID3D11Device* device, StreamClient& client, FrameStore& store, Page initialPage, float dpiScale);

    // Fenster wurde aktiviert: Einrichtung neu prüfen (z. B. nach einer Installation).
    void OnActivated();
    // Holt ein neues Bild für die Vorschau. true = es gab ein neues.
    bool UpdateVideo();
    void Draw();
    // Läuft gerade eine Animation (dann braucht die Hauptschleife weitere Bilder)?
    bool Animating() const { return m_animating; }

    VideoView& Video() { return m_video; }

private:
    void Recheck();
    void ShowPage(Page page);
    int OpenSetupItems() const;

    void DrawBackground(ImDrawList* dl, ImVec2 size, bool live);
    void DrawHeader(const StreamStatus& status, ImVec2 size);
    void DrawCameraPage(const StreamStatus& status, ImVec2 min, ImVec2 max);
    void DrawPreview(ImDrawList* dl, ImVec2 min, ImVec2 max, const StreamStatus& status, bool live);
    float DrawHero(ImDrawList* dl, ImVec2 pos, float width, const StreamStatus& status, bool live);
    void DrawStats(ImDrawList* dl, ImVec2 min, float width, float bottom, const StreamStatus& status);
    void DrawSteps(ImDrawList* dl, ImVec2 min, float width, float bottom);
    void DrawSetupPage(ImVec2 min, ImVec2 max);
    // Zeichnen jeweils eine Karte und geben die Höhe ihres Inhalts zurück.
    float DrawSetupList(ImDrawList* dl, ImVec2 pos, float width, float minHeight);
    float DrawSideloadGuide(ImDrawList* dl, ImVec2 pos, float width, float minHeight);

    StreamClient& m_client;
    FrameStore& m_store;
    VideoView m_video;
    SetupReport m_setup;
    Page m_page;
    std::chrono::steady_clock::time_point m_lastCheck;
    std::chrono::steady_clock::time_point m_pageShown;
    bool m_animating = false;
    float m_dpiScale = 1.0f;
    // Höhen vom letzten Bild: zum vertikalen Zentrieren und für gleich hohe Karten.
    float m_setupHeight = 0.0f;
    float m_listHeight = 0.0f;
    float m_guideHeight = 0.0f;
};
