#pragma once
#include "AutoRenew.h"
#include "FrameStore.h"
#include "SetupCheck.h"
#include "StreamClient.h"
#include "VideoView.h"
#include "VirtualCamera.h"

#include "imgui.h"

#include <chrono>
#include <map>
#include <string>

// Die Oberfläche im Stil von Eidolon: Kopfzeile mit Reitern, die Seite „Kamera“ mit Vorschau,
// Titel und Kennzahlen, und die Seite „Einrichtung“ als Karte mit einer Liste.
class App {
public:
    enum class Page { Camera, Setup };

    // dpiScale: Skalierung des Bildschirms (1,0 = 100 %). Die Oberfläche wächst zusätzlich mit dem Fenster.
    // selftest: nichts an Windows oder Sideloadly verändern.
    App(ID3D11Device* device, StreamClient& client, FrameStore& store, VirtualCamera& camera, Page initialPage,
        float dpiScale, bool selftest);

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
    // Verbunden und das iPhone meldet seinen Zustand: Vorschau mit Infozeile, rechts die Einstellungen.
    void DrawStudio(const StreamStatus& status, ImVec2 min, ImVec2 max, bool live);
    void DrawPreview(ImDrawList* dl, ImVec2 min, ImVec2 max, const StreamStatus& status, bool live);
    float DrawHero(ImDrawList* dl, ImVec2 pos, float width, const StreamStatus& status, bool live);
    // Hinweise (Signatur, Einrichtung) samt Knöpfen. Gibt die belegte Höhe zurück (0 = keine).
    float DrawNotes(ImDrawList* dl, ImVec2 pos, float width, const StreamStatus& status, bool live);
    float DrawInfoStrip(ImDrawList* dl, ImVec2 pos, float width, const StreamStatus& status, bool live);
    void DrawSettings(ImVec2 min, ImVec2 max);
    void DrawStats(ImDrawList* dl, ImVec2 min, float width, float bottom, const StreamStatus& status);
    void DrawSteps(ImDrawList* dl, ImVec2 min, float width, float bottom);
    void DrawSetupPage(ImVec2 min, ImVec2 max);
    // Zeichnen jeweils eine Karte und geben die Höhe ihres Inhalts zurück.
    float DrawSetupList(ImDrawList* dl, ImVec2 pos, float width, float minHeight);
    float DrawSideloadGuide(ImDrawList* dl, ImVec2 pos, float width, float minHeight);
    // Zeile „Virtuelle Kamera“ in der Einrichtungsliste. Gibt ihre Höhe zurück.
    float DrawCameraRow(ImDrawList* dl, float listLeft, float listRight, float textX, float y, bool separator);

    // Kamera-Zustand: eigene Änderungen gelten kurz vor dem gemeldeten Wert, bis das iPhone nachzieht.
    const JsonValue* CamValue(const std::string& key) const;
    bool CamHas(const std::string& key) const { return CamValue(key) != nullptr; }
    double CamNumber(const std::string& key, double fallback = 0.0) const;
    bool CamFlag(const std::string& key) const;
    std::string CamString(const std::string& key) const;
    void SetCamNumber(const std::string& key, double value);
    void SetCamFlag(const std::string& key, bool value);
    void SetCamString(const std::string& key, const std::string& value);
    // Schickt gesammelte Änderungen (höchstens alle 80 ms, force = sofort).
    void FlushCamCommands(bool force);

    struct LocalValue {
        JsonValue value;
        std::chrono::steady_clock::time_point until;
    };
    struct SectionState {
        bool open = true;
        float height = 0.0f;
    };

    StreamClient& m_client;
    FrameStore& m_store;
    VirtualCamera& m_camera;
    VirtualCamera::Status m_cameraStatus;  // je Bild einmal abgefragt
    VideoView m_video;
    SetupReport m_setup;
    AutoRenew m_renew;
    Page m_page;
    std::chrono::steady_clock::time_point m_lastCheck;
    std::chrono::steady_clock::time_point m_pageShown;
    std::chrono::steady_clock::time_point m_streamingSince;  // leer, solange kein Bild kommt
    bool m_animating = false;
    float m_dpiScale = 1.0f;
    // Höhen vom letzten Bild: zum vertikalen Zentrieren und für gleich hohe Karten.
    float m_setupHeight = 0.0f;
    float m_listHeight = 0.0f;
    float m_guideHeight = 0.0f;
    float m_stripHeight = 0.0f;

    std::map<std::string, JsonValue> m_cam;
    std::map<std::string, LocalValue> m_local;
    std::map<std::string, std::string> m_pending;  // Schlüssel → JSON-Wert
    std::chrono::steady_clock::time_point m_lastSend;
    std::map<std::string, SectionState> m_sections;
};
