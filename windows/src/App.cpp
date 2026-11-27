#include "App.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>

namespace {

const ImVec4 kGreen(0.36f, 0.80f, 0.50f, 1.0f);
const ImVec4 kOrange(0.97f, 0.67f, 0.25f, 1.0f);
const ImVec4 kRed(0.94f, 0.40f, 0.40f, 1.0f);
const ImVec4 kGrey(0.62f, 0.64f, 0.69f, 1.0f);
const ImVec4 kBlue(0.45f, 0.65f, 0.98f, 1.0f);

void StatusDot(const ImVec4& color) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 position = ImGui::GetCursorScreenPos();
    float lineHeight = ImGui::GetTextLineHeight();
    drawList->AddCircleFilled(ImVec2(position.x + lineHeight * 0.32f, position.y + lineHeight * 0.52f),
                              lineHeight * 0.27f, ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(lineHeight * 0.75f, lineHeight));
    ImGui::SameLine();
}

void MutedWrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

std::string FormatDate(int64_t unixSeconds) {
    time_t value = static_cast<time_t>(unixSeconds);
    tm local{};
    localtime_s(&local, &value);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%d.%m.%Y", &local);
    return buffer;
}

const char* LensLabel(const std::string& lens) {
    if (lens == "wide") return "Weitwinkel";
    if (lens == "ultraWide") return "Ultraweitwinkel";
    if (lens == "telephoto") return "Tele";
    if (lens == "front") return "Frontkamera";
    if (lens == "test") return "Testbild";
    return lens.empty() ? "–" : lens.c_str();
}

struct StateText {
    const char* title;
    const char* hint;
    ImVec4 color;
};

StateText DescribeState(const StreamStatus& status) {
    using State = StreamStatus::State;
    switch (status.state) {
    case State::Starting: return {"Startet …", "", kGrey};
    case State::NoAppleService:
        return {"Apple-USB-Dienst fehlt", "Installiere iTunes (siehe Einrichtung) und starte KabelCam neu.", kRed};
    case State::NoDevice:
        return {"Kein iPhone gefunden",
                "iPhone per Kabel anschließen, entsperren und „Diesem Computer vertrauen“ bestätigen.", kOrange};
    case State::AppNotReachable:
        if (status.direct) return {"Testquelle nicht erreichbar", "Läuft kabelcam_fakephone.exe?", kOrange};
        return {"iPhone gefunden", "Öffne jetzt die KabelCam-App auf dem iPhone. Sie muss im Vordergrund bleiben.",
                kOrange};
    case State::Connected: return {"Verbunden", "Warte auf das erste Bild …", kBlue};
    case State::Streaming: return {"Überträgt", "", kGreen};
    }
    return {"", "", kGrey};
}

}  // namespace

App::App(ID3D11Device* device, StreamClient& client, FrameStore& store, const AppFonts& fonts, float scale)
    : m_client(client), m_store(store), m_fonts(fonts), m_scale(scale) {
    m_video.Init(device);
    Recheck();
}

void App::Recheck() {
    m_setup = RunSetupCheck();
    m_lastCheck = std::chrono::steady_clock::now();
}

void App::OnActivated() {
    // Nach einer Installation kommt man ins Fenster zurück; dann neu prüfen (höchstens 1x pro Sekunde).
    if (std::chrono::steady_clock::now() - m_lastCheck > std::chrono::seconds(1)) Recheck();
}

bool App::UpdateVideo() {
    return m_video.Update(m_store);
}

void App::SectionTitle(const char* text) {
    ImGui::Spacing();
    if (m_fonts.bold) ImGui::PushFont(m_fonts.bold);
    ImGui::SeparatorText(text);
    if (m_fonts.bold) ImGui::PopFont();
}

void App::Draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("KabelCam", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    const StreamStatus status = m_client.Status();
    const float sidebarWidth = std::min(400.0f * m_scale, viewport->WorkSize.x * 0.45f);
    const ImVec2 available = ImGui::GetContentRegionAvail();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::BeginChild("video", ImVec2(available.x - sidebarWidth, available.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawVideo(status);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * m_scale, 14.0f * m_scale));
    ImGui::BeginChild("sidebar", ImVec2(sidebarWidth, available.y), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawConnection(status);
    DrawSignature(status);
    DrawSetup();
    DrawVirtualCamera();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
}

void App::DrawVideo(const StreamStatus& status) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const bool live = status.state == StreamStatus::State::Streaming && m_video.HasFrame();

    if (live && m_video.Width() > 0 && m_video.Height() > 0) {
        const float videoWidth = static_cast<float>(m_video.Width());
        const float videoHeight = static_cast<float>(m_video.Height());
        const float fit = std::min(size.x / videoWidth, size.y / videoHeight);
        const ImVec2 drawn(videoWidth * fit, videoHeight * fit);
        const ImVec2 topLeft(origin.x + (size.x - drawn.x) * 0.5f, origin.y + (size.y - drawn.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(m_video.Srv()), topLeft,
                                             ImVec2(topLeft.x + drawn.x, topLeft.y + drawn.y));
        return;
    }

    // Kein Bild: Hinweis in der Mitte.
    const StateText text = DescribeState(status);
    const float wrapWidth = std::min(size.x * 0.8f, 520.0f * m_scale);
    if (m_fonts.title) ImGui::PushFont(m_fonts.title);
    const ImVec2 titleSize = ImGui::CalcTextSize(text.title);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + (size.x - titleSize.x) * 0.5f, origin.y + size.y * 0.42f));
    ImGui::TextColored(text.color, "%s", text.title);
    if (m_fonts.title) ImGui::PopFont();

    if (text.hint[0] != '\0') {
        const ImVec2 hintSize = ImGui::CalcTextSize(text.hint, nullptr, false, wrapWidth);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + (size.x - hintSize.x) * 0.5f,
                                         origin.y + size.y * 0.42f + titleSize.y + 12.0f * m_scale));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TextUnformatted(text.hint);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
}

void App::DrawConnection(const StreamStatus& status) {
    SectionTitle(status.direct ? "Testquelle" : "iPhone");
    const StateText text = DescribeState(status);
    StatusDot(text.color);
    if (m_fonts.bold) ImGui::PushFont(m_fonts.bold);
    ImGui::TextUnformatted(text.title);
    if (m_fonts.bold) ImGui::PopFont();
    if (!status.detail.empty()) MutedWrapped(status.detail);
    else if (text.hint[0] != '\0' && status.state != StreamStatus::State::Streaming) MutedWrapped(text.hint);

    if (status.state == StreamStatus::State::Streaming || status.state == StreamStatus::State::Connected) {
        if (status.width > 0) {
            ImGui::Text("%d × %d · %d FPS · %.0f Mbit/s", status.width, status.height, status.fps,
                        static_cast<double>(status.bitrate) / 1e6);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::Text("Empfangen: %.0f FPS · %.1f Mbit/s", status.measuredFps, status.measuredMbps);
        ImGui::Text("Objektiv: %s", LensLabel(status.lens));
        ImGui::Text("Dekodierung: %s", status.hardwareDecoding ? "Grafikkarte" : "Prozessor (langsamer)");
        ImGui::PopStyleColor();
    }
}

void App::DrawSignature(const StreamStatus& status) {
    if (status.signatureExpires <= 0) return;
    SectionTitle("Signatur");
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const double daysLeft = static_cast<double>(status.signatureExpires - now) / 86400.0;
    const std::string date = FormatDate(status.signatureExpires);

    if (daysLeft < 0) {
        StatusDot(kRed);
        ImGui::TextUnformatted("Abgelaufen");
        MutedWrapped("Spiele die App mit Sideloadly neu auf, dann läuft sie wieder 7 Tage.");
    } else {
        const int days = static_cast<int>(std::floor(daysLeft));
        StatusDot(days <= 2 ? kOrange : kGreen);
        ImGui::Text("Gültig bis %s", date.c_str());
        if (days == 0) MutedWrapped("Läuft heute ab. Jetzt mit Sideloadly erneuern.");
        else if (days == 1) MutedWrapped("Noch 1 Tag. Am besten jetzt mit Sideloadly erneuern.");
        else if (days <= 2) MutedWrapped("Noch " + std::to_string(days) + " Tage. Bald mit Sideloadly erneuern.");
        else MutedWrapped("Noch " + std::to_string(days) + " Tage.");
    }
    if (!m_setup.sideloadlyExe.empty() && daysLeft <= 2) {
        if (ImGui::Button("Sideloadly öffnen")) OpenTarget(m_setup.sideloadlyExe);
    }
}

void App::DrawSetup() {
    SectionTitle("Einrichtung");
    for (size_t i = 0; i < m_setup.items.size(); ++i) {
        const SetupItem& item = m_setup.items[i];
        ImGui::PushID(static_cast<int>(i));
        const char* icon = "✓";
        ImVec4 color = kGreen;
        switch (item.status) {
        case SetupItem::Status::Ok: break;
        case SetupItem::Status::Missing: icon = "✗"; color = kRed; break;
        case SetupItem::Status::Warning: icon = "!"; color = kOrange; break;
        case SetupItem::Status::Info: icon = "i"; color = kBlue; break;
        }
        ImGui::TextColored(color, "%s", icon);
        ImGui::SameLine(0.0f, 10.0f * m_scale);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(item.title.c_str());
        if (!item.detail.empty()) MutedWrapped(item.detail);
        if (!item.actionLabel.empty()) {
            if (ImGui::SmallButton(item.actionLabel.c_str())) OpenTarget(item.actionTarget);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (ImGui::Button("Erneut prüfen")) Recheck();
}

void App::DrawVirtualCamera() {
    SectionTitle("Virtuelle Kamera");
    MutedWrapped("Kommt im nächsten Schritt. Dann erscheint „KabelCam“ als Kamera in OBS, Zoom, Discord und Co.");
}
