#include "App.h"

#include "TextUtil.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace Theme;

namespace {

using Clock = std::chrono::steady_clock;

std::string Decimal(double value, int digits) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    for (char& c : buffer) {
        if (c == '.') c = ',';
    }
    return buffer;
}

// Kalendertage bis zum Datum (heute = 0), unabhängig von der Uhrzeit.
int CalendarDaysUntil(int64_t unixSeconds) {
    time_t now = time(nullptr);
    time_t then = static_cast<time_t>(unixSeconds);
    tm today{};
    tm target{};
    localtime_s(&today, &now);
    localtime_s(&target, &then);
    for (tm* day : {&today, &target}) {
        day->tm_hour = 12;
        day->tm_min = 0;
        day->tm_sec = 0;
        day->tm_isdst = -1;
    }
    return static_cast<int>(std::lround(difftime(mktime(&target), mktime(&today)) / 86400.0));
}

std::string ShortDate(int64_t unixSeconds) {
    time_t value = static_cast<time_t>(unixSeconds);
    tm local{};
    localtime_s(&local, &value);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%d.%m.", &local);
    return buffer;
}

std::string LensLabel(const std::string& lens) {
    if (lens == "wide") return "Weitwinkel";
    if (lens == "ultraWide") return "Ultraweitwinkel";
    if (lens == "telephoto") return "Tele";
    if (lens == "front") return "Frontkamera";
    if (lens == "test") return "Testbild";
    return lens;
}

struct StateText {
    const char* title;
    const char* hint;
    const char* chip;
    Tone tone;
};

StateText Describe(const StreamStatus& status) {
    using State = StreamStatus::State;
    switch (status.state) {
    case State::Starting: return {"Startet …", "", "Startet", Tone::Neutral};
    case State::NoAppleService:
        return {"Apple-USB-Dienst fehlt",
                "Ohne ihn erreicht KrakznCam das iPhone nicht. Er kommt mit iTunes, den Download findest du unter "
                "Einrichtung.",
                "USB-Dienst fehlt", Tone::Bad};
    case State::NoDevice:
        return {"Kein iPhone gefunden",
                "Schließ das iPhone per Kabel an, entsperre es und bestätige „Diesem Computer vertrauen“.",
                "Kein iPhone", Tone::Warn};
    case State::AppNotReachable:
        if (status.direct) return {"Testquelle nicht erreichbar", "Starte krakzncam_fakephone.exe.", "Keine Quelle", Tone::Warn};
        return {"KrakznCam öffnen",
                "Das iPhone ist angeschlossen. Öffne jetzt die KrakznCam-App, sie muss im Vordergrund bleiben.",
                "App öffnen", Tone::Warn};
    case State::Connected: return {"Verbunden", "Warte auf das erste Bild …", "Verbunden", Tone::Neutral};
    case State::Streaming: return {"Überträgt", "", "Live", Tone::Ok};
    }
    return {"", "", "", Tone::Neutral};
}

float PagePadding(float width) {
    return std::clamp(width * 0.04f, Px(28.0f), Px(64.0f));
}

ImTextureRef TextureOf(ID3D11ShaderResourceView* srv) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv)));
}

// Hinweis-Kasten in Gold, wie Eidolons .hero-note.warn. Gibt die Höhe zurück.
float WarnNote(ImDrawList* dl, ImVec2 pos, float width, std::string_view text) {
    const ImVec2 padding(Px(14.0f), Px(11.0f));
    const float size = Px(14.5f);
    const float textHeight = Measure(GetFonts().regular, size, text, width - padding.x * 2.0f).y;
    const float height = textHeight + padding.y * 2.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), A(WithAlpha(kGold, 0.10f)), Px(9.0f));
    Text(dl, GetFonts().regular, size, pos + padding, kGold, text, width - padding.x * 2.0f);
    return height;
}

// Neutraler Hinweis-Kasten mit Info-Symbol. Gibt die Höhe zurück.
float InfoNote(ImDrawList* dl, ImVec2 pos, float width, std::string_view text) {
    const ImVec2 padding(Px(14.0f), Px(11.0f));
    const float size = Px(14.5f);
    const float iconSize = Px(15.0f);
    const float textX = pos.x + padding.x + iconSize + Px(10.0f);
    const float textWidth = pos.x + width - padding.x - textX;
    const float textHeight = Measure(GetFonts().regular, size, text, textWidth).y;
    const float height = textHeight + padding.y * 2.0f;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), A(White(0.07f)), Px(9.0f));
    DrawIcon(dl, Icon::Info, ImVec2(pos.x + padding.x + iconSize * 0.5f, pos.y + padding.y + Px(9.5f)), iconSize, kText2);
    Text(dl, GetFonts().regular, size, ImVec2(textX, pos.y + padding.y), kText2, text, textWidth);
    return height;
}

std::string LongDate(int64_t unixSeconds) {
    time_t value = static_cast<time_t>(unixSeconds);
    tm local{};
    localtime_s(&local, &value);
    char buffer[16];
    strftime(buffer, sizeof(buffer), "%d.%m.%Y", &local);
    return buffer;
}

// Beschriftung in gesperrten Großbuchstaben (Bahnschrift).
float Kicker(ImDrawList* dl, ImVec2 pos, std::string_view text, float size = 11.5f) {
    Spaced(dl, GetFonts().brand, Px(size), pos, kText3, text, 0.2f);
    return Px(size) * 1.25f;
}

struct ItemLook {
    char32_t icon;
    ImU32 color;
};

ItemLook LookFor(const SetupItem& item) {
    char32_t icon = Icon::Info;
    if (item.key == "windows") icon = Icon::Monitor;
    else if (item.key == "itunes") icon = 0xE8D6;  // Audio
    else if (item.key == "icloud") icon = 0xE753;  // Cloud
    else if (item.key == "usb") icon = Icon::Usb;
    else if (item.key == "sideloadly") icon = Icon::Phone;
    else if (item.key == "appledevices") icon = 0xE772;  // Devices
    switch (item.status) {
    case SetupItem::Status::Ok: return {icon, kGreen};
    case SetupItem::Status::Missing: return {icon, kRed};
    case SetupItem::Status::Warning: return {icon, kGold};
    case SetupItem::Status::Info: return {icon, kAccent};
    }
    return {icon, kText2};
}

}  // namespace

App::App(ID3D11Device* device, StreamClient& client, FrameStore& store, Page initialPage, float dpiScale,
         bool selftest)
    : m_client(client),
      m_store(store),
      m_renew(!selftest),
      m_page(initialPage),
      m_pageShown(Clock::now()),
      m_dpiScale(dpiScale) {
    m_video.Init(device);
    Recheck();
}

void App::Recheck() {
    m_setup = RunSetupCheck();
    m_lastCheck = Clock::now();
}

void App::OnActivated() {
    // Nach einer Installation kommt man ins Fenster zurück; dann neu prüfen (höchstens 1x pro Sekunde).
    if (Clock::now() - m_lastCheck > std::chrono::seconds(1)) Recheck();
}

void App::ShowPage(Page page) {
    if (page == m_page) return;
    m_page = page;
    m_pageShown = Clock::now();
    if (page == Page::Setup) Recheck();
}

int App::OpenSetupItems() const {
    int open = 0;
    for (const SetupItem& item : m_setup.items) {
        if (item.status == SetupItem::Status::Missing || item.status == SetupItem::Status::Warning) ++open;
    }
    return open;
}

bool App::UpdateVideo() {
    return m_video.Update(m_store);
}

void App::Draw() {
    BeginFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 size = viewport->Size;
    // Wie bei Eidolon wächst die Oberfläche mit dem Fenster (Bezug: 1280 × 760), damit die
    // Proportionen im Vollbild dieselben sind. Nie kleiner als 80 % der Bildschirm-Skalierung.
    SetScale(std::max(m_dpiScale * 0.8f, std::min(size.x / 1280.0f, size.y / 760.0f)));
    const StreamStatus status = m_client.Status();
    // Das Ablaufdatum der Testquelle ist ausgedacht und wird nie übernommen.
    m_renew.Update(status.direct ? 0 : status.signatureExpires, m_setup);
    const bool live = status.state == StreamStatus::State::Streaming && m_video.HasFrame();

    DrawBackground(ImGui::GetBackgroundDrawList(), size, live);

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("KrakznCam", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    DrawHeader(status, size);

    // Seitenwechsel: Inhalt hebt sich leicht und blendet ein (wie Eidolons „rise“).
    const float since = std::chrono::duration<float>(Clock::now() - m_pageShown).count();
    const bool revealing = Animations() && since < 0.55f;
    const float reveal = revealing ? Ease(since / 0.55f) : 1.0f;
    PushAlpha(reveal);
    const float rise = (1.0f - reveal) * Px(12.0f);
    const ImVec2 min(0.0f, Px(78.0f) + rise);
    const ImVec2 max(size.x, size.y + rise);
    if (m_page == Page::Camera) DrawCameraPage(status, min, max);
    else DrawSetupPage(min, max);
    PopAlpha();

    ImGui::End();
    m_animating = Theme::Animating() || revealing;
}

void App::DrawBackground(ImDrawList* dl, ImVec2 size, bool live) {
    Backdrop(dl, ImVec2(0.0f, 0.0f), size);
    // Während der Übertragung leuchtet das Kamerabild weich dahinter, wie Eidolons Key-Art:
    // eine winzige Fassung des Bildes (siehe VideoView), groß gezogen und damit unscharf.
    const float ambient = Animate(0x6B1A7E01u, live ? 1.0f : 0.0f, 3.0f);
    ID3D11ShaderResourceView* glow = m_video.AmbientSrv() ? m_video.AmbientSrv() : m_video.Srv();
    if (ambient > 0.01f && m_video.HasFrame() && glow) {
        const float aspect = static_cast<float>(m_video.Width()) / static_cast<float>(std::max(1u, m_video.Height()));
        const float fill = std::max(size.x, size.y * aspect) * 1.06f;
        const ImVec2 drawn(fill, fill / aspect);
        const ImVec2 topLeft((size.x - drawn.x) * 0.5f, (size.y - drawn.y) * 0.5f);
        dl->AddImage(TextureOf(glow), topLeft, topLeft + drawn, ImVec2(0, 0), ImVec2(1, 1), White(0.5f * ambient));
        const float split = size.y * 0.3f;
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, split), WithAlpha(kBg, 0.72f * ambient),
                                    WithAlpha(kBg, 0.72f * ambient), WithAlpha(kBg, 0.42f * ambient),
                                    WithAlpha(kBg, 0.42f * ambient));
        dl->AddRectFilledMultiColor(ImVec2(0, split), size, WithAlpha(kBg, 0.42f * ambient),
                                    WithAlpha(kBg, 0.42f * ambient), WithAlpha(kBg, 0.9f * ambient),
                                    WithAlpha(kBg, 0.9f * ambient));
    }
}

void App::DrawHeader(const StreamStatus& status, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = PagePadding(size.x);
    const float centerY = Px(40.0f);
    const float tabHeight = Measure(GetFonts().regular, Px(18.0f), "Kamera").y + Px(12.0f);

    float x = pad - Px(14.0f);
    float width = 0.0f;
    if (Tab("##tab_camera", "Kamera", ImVec2(x, centerY - tabHeight * 0.5f), m_page == Page::Camera, false, width)) {
        ShowPage(Page::Camera);
    }
    x += width + Px(4.0f);
    if (Tab("##tab_setup", "Einrichtung", ImVec2(x, centerY - tabHeight * 0.5f), m_page == Page::Setup,
            OpenSetupItems() > 0, width)) {
        ShowPage(Page::Setup);
    }

    // Rechts: Status und Uhr.
    char clock[8];
    const time_t now = time(nullptr);
    tm local{};
    localtime_s(&local, &now);
    strftime(clock, sizeof(clock), "%H:%M", &local);
    const float clockSize = Px(24.0f);
    const ImVec2 clockText = Measure(GetFonts().light, clockSize, clock);
    const float right = size.x - pad;
    Text(dl, GetFonts().light, clockSize, ImVec2(right - clockText.x, centerY - clockText.y * 0.5f), kText, clock);

    const StateText state = Describe(status);
    const float chipWidth = ChipWidth(state.chip, true);
    Chip(dl, ImVec2(right - clockText.x - Px(20.0f) - chipWidth, centerY - ChipHeight() * 0.5f), state.chip,
         state.tone, true);
}

void App::DrawCameraPage(const StreamStatus& status, ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool live = status.state == StreamStatus::State::Streaming && m_video.HasFrame();
    const float pad = PagePadding(max.x - min.x);
    const float gap = Px(40.0f);
    const float bottom = max.y - pad * 0.8f;

    const float column = std::clamp((max.x - min.x) * 0.31f, Px(330.0f), Px(440.0f));
    const float availableWidth = (max.x - pad) - (min.x + pad) - column - gap;
    const float availableHeight = bottom - (min.y + Px(6.0f));
    const float cardWidth = std::max(Px(200.0f), std::min(availableWidth, availableHeight * 16.0f / 9.0f));
    const float cardHeight = cardWidth * 9.0f / 16.0f;
    // Bleibt Platz, sitzt der Inhalt mittig (horizontal) und etwas über der Mitte (vertikal).
    const float top = min.y + Px(6.0f) + std::max(0.0f, availableHeight - cardHeight) * 0.42f;
    const float compositionWidth = cardWidth + gap + column;
    const float left = min.x + std::max(pad, ((max.x - min.x) - compositionWidth) * 0.5f);
    const ImVec2 cardMin(left, top);
    const ImVec2 cardMax(cardMin.x + cardWidth, top + cardHeight);
    DrawPreview(dl, cardMin, cardMax, status, live);

    const float columnX = cardMax.x + gap;
    const float columnWidth = column;
    const float heroBottom = DrawHero(dl, ImVec2(columnX, top + Px(4.0f)), columnWidth, status, live);
    const bool connected =
        status.state == StreamStatus::State::Streaming || status.state == StreamStatus::State::Connected;
    const ImVec2 cardTop(columnX, heroBottom + Px(22.0f));
    if (connected) DrawStats(dl, cardTop, columnWidth, cardMax.y, status);
    else DrawSteps(dl, cardTop, columnWidth, cardMax.y);
}

void App::DrawPreview(ImDrawList* dl, ImVec2 min, ImVec2 max, const StreamStatus& status, bool live) {
    const float rounding = Px(16.0f);
    Shadow(dl, min, max, rounding, Px(36.0f), 0.65f);
    dl->AddRectFilled(min, max, A(IM_COL32(0, 0, 0, 255)), rounding);

    if (live) {
        const float width = static_cast<float>(m_video.Width());
        const float height = static_cast<float>(m_video.Height());
        const float fit = std::min((max.x - min.x) / width, (max.y - min.y) / height);
        const ImVec2 drawn(width * fit, height * fit);
        const ImVec2 topLeft(min.x + ((max.x - min.x) - drawn.x) * 0.5f, min.y + ((max.y - min.y) - drawn.y) * 0.5f);
        dl->AddImageRounded(TextureOf(m_video.Srv()), topLeft, topLeft + drawn, ImVec2(0, 0), ImVec2(1, 1),
                            A(IM_COL32_WHITE), rounding);
        LiveRing(dl, min, max, rounding);
        return;
    }

    dl->AddRectFilled(min, max, A(White(0.035f)), rounding);
    const ImVec2 center = (min + max) * 0.5f;
    const bool phoneMissing = status.state == StreamStatus::State::NoDevice;
    DrawIcon(dl, phoneMissing ? Icon::Phone : Icon::Video, ImVec2(center.x, center.y - Px(16.0f)), Px(44.0f), kText3);
    const char* label = status.state == StreamStatus::State::Connected ? "Warte auf das Bild …" : "Noch kein Bild";
    const ImVec2 text = Measure(GetFonts().light, Px(17.0f), label);
    Text(dl, GetFonts().light, Px(17.0f), ImVec2(center.x - text.x * 0.5f, center.y + Px(22.0f)), kText3, label);
}

float App::DrawHero(ImDrawList* dl, ImVec2 pos, float width, const StreamStatus& status, bool live) {
    const Fonts& fonts = GetFonts();
    const StateText state = Describe(status);
    float y = pos.y;

    y += Kicker(dl, ImVec2(pos.x, y), status.direct ? "TESTQUELLE" : "IPHONE") + Px(8.0f);
    y += Text(dl, fonts.light, Px(38.0f), ImVec2(pos.x, y), kText, state.title, width) + Px(14.0f);

    // Chips mit Format und Objektiv.
    if (live || status.state == StreamStatus::State::Connected) {
        std::vector<std::string> chips;
        if (status.width > 0) chips.push_back(std::to_string(status.width) + " × " + std::to_string(status.height));
        if (status.fps > 0) chips.push_back(std::to_string(status.fps) + " FPS");
        if (!status.lens.empty()) chips.push_back(LensLabel(status.lens));
        chips.push_back("H.264");
        float x = pos.x;
        for (const std::string& chip : chips) {
            const float chipWidth = ChipWidth(chip, false);
            if (x > pos.x && x + chipWidth > pos.x + width) {
                x = pos.x;
                y += ChipHeight() + Px(7.0f);
            }
            x += Chip(dl, ImVec2(x, y), chip, Tone::Neutral) + Px(7.0f);
        }
        y += ChipHeight() + Px(18.0f);
    }

    if (state.hint[0] != '\0') {
        y += Text(dl, fonts.light, Px(16.0f), ImVec2(pos.x, y), kText2, state.hint, width) + Px(18.0f);
    }

    // Hinweise: Signatur läuft ab, oder auf dem PC fehlt noch etwas.
    bool showSideloadly = false;
    bool showSetup = false;
    if (status.signatureExpires > 0) {
        const int days = CalendarDaysUntil(status.signatureExpires);
        const bool expired = status.signatureExpires < static_cast<int64_t>(time(nullptr));
        const AutoRenew::State renew = m_renew.GetState();
        if (renew == AutoRenew::State::Renewing) {
            y += InfoNote(dl, ImVec2(pos.x, y), width,
                          "Die Signatur läuft bald ab. Sideloadly erneuert sie gerade automatisch, die App auf dem "
                          "iPhone startet dabei kurz neu.") +
                 Px(16.0f);
        } else if ((expired || days <= 2) && renew == AutoRenew::State::Waiting) {
            y += InfoNote(dl, ImVec2(pos.x, y), width,
                          "Die Signatur läuft am " + ShortDate(status.signatureExpires) +
                              " ab. KrakznCam lässt sie rechtzeitig von Sideloadly erneuern.") +
                 Px(16.0f);
        } else if (expired || days <= 2) {
            std::string note;
            if (expired) note = "Die Signatur ist abgelaufen. Spiele die App mit Sideloadly neu auf.";
            else if (days == 0) note = "Die Signatur läuft heute ab. Erneuere sie jetzt mit Sideloadly.";
            else if (days == 1) note = "Die Signatur läuft morgen ab. Erneuere sie mit Sideloadly.";
            else note = "Die Signatur läuft am " + ShortDate(status.signatureExpires) + " ab. Erneuere sie bald mit Sideloadly.";
            y += WarnNote(dl, ImVec2(pos.x, y), width, note) + Px(16.0f);
            showSideloadly = !m_setup.sideloadlyExe.empty();
        }
    }
    if (!live && OpenSetupItems() > 0) {
        y += WarnNote(dl, ImVec2(pos.x, y), width, "Auf diesem PC fehlt noch etwas für Sideloadly.") + Px(16.0f);
        showSetup = true;
    }

    float x = pos.x;
    if (showSideloadly) {
        const float buttonWidth = ButtonWidth("Sideloadly öffnen", Icon::OpenExternal, ButtonSize::Normal);
        if (Button("##hero_sideloadly", "Sideloadly öffnen", Icon::OpenExternal, ImVec2(x, y))) {
            OpenTarget(m_setup.sideloadlyExe);
        }
        x += buttonWidth + Px(12.0f);
    }
    if (showSetup) {
        if (Button("##hero_setup", "Zur Einrichtung", Icon::Settings, ImVec2(x, y))) ShowPage(Page::Setup);
    }
    if (showSideloadly || showSetup) y += ButtonHeight(ButtonSize::Normal);
    return y;
}

void App::DrawStats(ImDrawList* dl, ImVec2 min, float width, float bottom, const StreamStatus& status) {
    const Fonts& fonts = GetFonts();
    struct Stat {
        std::string value;
        const char* label;
    };
    std::vector<Stat> stats;
    stats.push_back({Decimal(status.measuredFps, 0) + " FPS", "EMPFANGEN"});
    stats.push_back({Decimal(status.measuredMbps, 1) + " Mbit/s", "BITRATE"});
    stats.push_back({status.hardwareDecoding ? "Grafikkarte" : "Prozessor", "DEKODIERUNG"});
    float signatureFraction = -1.0f;
    if (status.signatureExpires > 0) {
        const int days = CalendarDaysUntil(status.signatureExpires);
        const bool expired = status.signatureExpires < static_cast<int64_t>(time(nullptr));
        std::string value = expired ? "Abgelaufen" : days == 0 ? "Heute" : days == 1 ? "1 Tag" : std::to_string(days) + " Tage";
        stats.push_back({value, "SIGNATUR"});
        const double secondsLeft = static_cast<double>(status.signatureExpires - static_cast<int64_t>(time(nullptr)));
        signatureFraction = static_cast<float>(std::clamp(secondsLeft / (7.0 * 86400.0), 0.0, 1.0));
    } else {
        stats.push_back({"–", "SIGNATUR"});
    }

    const ImVec2 padding(Px(24.0f), Px(22.0f));
    const float valueSize = Px(26.0f);
    const float labelSize = Px(10.5f);
    const float rowGap = Px(18.0f);
    const float cell = Measure(fonts.light, valueSize, "0").y + Px(5.0f) + labelSize * 1.25f;
    const int rows = static_cast<int>((stats.size() + 1) / 2);
    float height = padding.y * 2.0f + rows * cell + (rows - 1) * rowGap;
    if (signatureFraction >= 0.0f) height += Px(20.0f) + Px(4.0f);

    const float cardTop = std::max(min.y, bottom - height);
    const ImVec2 cardMin(min.x, cardTop);
    const ImVec2 cardMax(min.x + width, cardTop + height);
    Card(dl, cardMin, cardMax, Px(14.0f));

    const float columnWidth = (width - padding.x * 2.0f - Px(20.0f)) * 0.5f;
    for (size_t i = 0; i < stats.size(); ++i) {
        const float x = cardMin.x + padding.x + static_cast<float>(i % 2) * (columnWidth + Px(20.0f));
        const float y = cardMin.y + padding.y + static_cast<float>(i / 2) * (cell + rowGap);
        const float valueHeight = Text(dl, fonts.light, valueSize, ImVec2(x, y), kText, stats[i].value);
        Spaced(dl, fonts.brand, labelSize, ImVec2(x, y + valueHeight + Px(5.0f)), kText3, stats[i].label, 0.14f);
    }
    if (signatureFraction >= 0.0f) {
        const float y = cardMax.y - padding.y - Px(4.0f);
        Meter(dl, ImVec2(cardMin.x + padding.x, y), ImVec2(cardMax.x - padding.x, y + Px(4.0f)), signatureFraction);
    }
}

void App::DrawSteps(ImDrawList* dl, ImVec2 min, float width, float bottom) {
    const Fonts& fonts = GetFonts();
    const char* steps[] = {
        "iPhone per Kabel anschließen und entsperren",
        "KrakznCam-App auf dem iPhone öffnen",
        "Das Bild erscheint links in der Vorschau",
    };
    const ImVec2 padding(Px(24.0f), Px(22.0f));
    const float textSize = Px(15.0f);
    const float circle = Px(26.0f);
    const float textX = padding.x + circle + Px(14.0f);
    const float textWidth = width - textX - padding.x;

    float height = padding.y * 2.0f + Px(11.5f) * 1.25f + Px(14.0f);
    std::vector<float> rowHeights;
    for (const char* step : steps) {
        const float rowHeight = std::max(circle, Measure(fonts.regular, textSize, step, textWidth).y);
        rowHeights.push_back(rowHeight);
        height += rowHeight + Px(12.0f);
    }
    height -= Px(12.0f);

    const float cardTop = std::max(min.y, bottom - height);
    const ImVec2 cardMin(min.x, cardTop);
    Card(dl, cardMin, ImVec2(min.x + width, cardTop + height), Px(14.0f));
    float y = cardMin.y + padding.y;
    y += Kicker(dl, ImVec2(cardMin.x + padding.x, y), "SO GEHT'S", 10.5f) + Px(14.0f);
    for (size_t i = 0; i < std::size(steps); ++i) {
        const ImVec2 center(cardMin.x + padding.x + circle * 0.5f, y + circle * 0.5f);
        dl->AddCircleFilled(center, circle * 0.5f, A(White(0.10f)));
        const std::string number = std::to_string(i + 1);
        const ImVec2 numberSize = Measure(fonts.semibold, Px(13.5f), number);
        Text(dl, fonts.semibold, Px(13.5f), center - numberSize * 0.5f, kText, number);
        const float lineHeight = Measure(fonts.regular, textSize, "A").y;
        const float textTop = rowHeights[i] > circle ? y : y + (circle - lineHeight) * 0.5f;
        Text(dl, fonts.regular, textSize, ImVec2(cardMin.x + textX, textTop), kText2, steps[i], textWidth);
        y += rowHeights[i] + Px(12.0f);
    }
}

void App::DrawSetupPage(ImVec2 min, ImVec2 max) {
    const float pad = PagePadding(max.x - min.x);
    const float gap = Px(28.0f);
    const float available = (max.x - min.x) - pad * 2.0f;
    // Breit genug: Liste und Anleitung nebeneinander, sonst untereinander.
    const bool sideBySide = available >= Px(980.0f);
    const float totalWidth = sideBySide ? std::min(available, Px(1240.0f)) : std::min(available, Px(820.0f));
    const float listWidth = sideBySide ? (totalWidth - gap) * 0.55f : totalWidth;
    const float guideWidth = sideBySide ? totalWidth - gap - listWidth : totalWidth;
    const float left = min.x + ((max.x - min.x) - totalWidth) * 0.5f;

    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild("##setup_scroll", max - min, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // Etwas über der Mitte, solange alles passt (Höhe aus dem letzten Bild).
    const float availableHeight = (max.y - min.y) - pad * 0.8f - Px(6.0f);
    const float top = origin.y + Px(6.0f) + std::max(0.0f, availableHeight - m_setupHeight) * 0.38f;

    if (sideBySide) {
        const float sameHeight = std::max(m_listHeight, m_guideHeight);
        m_listHeight = DrawSetupList(dl, ImVec2(left, top), listWidth, sameHeight);
        m_guideHeight = DrawSideloadGuide(dl, ImVec2(left + listWidth + gap, top), guideWidth, sameHeight);
        m_setupHeight = std::max(m_listHeight, m_guideHeight);
    } else {
        m_listHeight = DrawSetupList(dl, ImVec2(left, top), listWidth, 0.0f);
        m_guideHeight = DrawSideloadGuide(dl, ImVec2(left, top + m_listHeight + gap), guideWidth, 0.0f);
        m_setupHeight = m_listHeight + gap + m_guideHeight;
    }

    // Platz unter den Karten, damit man bis ans Ende scrollen kann.
    ImGui::SetCursorScreenPos(ImVec2(min.x, top + m_setupHeight + pad * 0.6f));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    ImGui::EndChild();
}

float App::DrawSetupList(ImDrawList* dl, ImVec2 pos, float width, float minHeight) {
    const Fonts& fonts = GetFonts();
    const ImVec2 padding(Px(28.0f), Px(26.0f));
    const float listLeft = pos.x + padding.x - Px(12.0f);
    const float listRight = pos.x + width - padding.x + Px(12.0f);
    const float iconBox = Px(44.0f);
    const float textX = listLeft + Px(12.0f) + iconBox + Px(16.0f);
    const float titleSize = Px(17.0f);
    const float descSize = Px(13.5f);

    const std::vector<SetupItem>& items = m_setup.items;
    auto trailingWidth = [&](const SetupItem& item) {
        if (!item.actionLabel.empty()) return ButtonWidth(item.actionLabel, Icon::OpenExternal, ButtonSize::Small);
        if (item.status == SetupItem::Status::Ok) return ChipWidth("Bereit", false);
        return 0.0f;
    };
    auto textWidthFor = [&](const SetupItem& item) {
        const float trailing = trailingWidth(item);
        return listRight - Px(12.0f) - textX - (trailing > 0 ? trailing + Px(16.0f) : 0.0f);
    };
    auto contentHeight = [&](const SetupItem& item) {
        float content = Measure(fonts.light, titleSize, item.title).y;
        if (!item.detail.empty()) content += Px(3.0f) + Measure(fonts.regular, descSize, item.detail, textWidthFor(item)).y;
        return content;
    };

    // Inhalt auf Ebene 1, die Karte danach auf Ebene 0 darunter (ihre Höhe steht erst am Ende fest).
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    float y = pos.y + padding.y;
    const float titleHeight = Measure(fonts.light, Px(28.0f), "Einrichtung").y;
    Text(dl, fonts.light, Px(28.0f), ImVec2(pos.x + padding.x, y), kText, "Einrichtung");
    const int open = OpenSetupItems();
    const std::string summary =
        open == 0 ? "Alles bereit" : open == 1 ? "1 Punkt offen" : std::to_string(open) + " Punkte offen";
    const float summaryWidth = ChipWidth(summary, true);
    Chip(dl, ImVec2(pos.x + width - padding.x - summaryWidth, y + (titleHeight - ChipHeight()) * 0.5f), summary,
         open == 0 ? Tone::Ok : Tone::Warn, true);
    y += titleHeight + Px(4.0f);
    y += Text(dl, fonts.light, Px(15.0f), ImVec2(pos.x + padding.x, y), kText2,
              "Was KrakznCam und Sideloadly auf diesem PC brauchen.", width - padding.x * 2.0f) +
         Px(16.0f);

    for (size_t i = 0; i < items.size(); ++i) {
        const SetupItem& item = items[i];
        const float content = contentHeight(item);
        const float height = std::max(iconBox, content) + Px(13.0f) * 2.0f;
        if (i > 0) {
            dl->AddRectFilled(ImVec2(listLeft + Px(12.0f), y), ImVec2(listRight - Px(12.0f), y + 1.0f),
                              A(White(0.07f)));
        }
        const ItemLook look = LookFor(item);
        const ImVec2 iconMin(listLeft + Px(12.0f), y + (height - iconBox) * 0.5f);
        dl->AddRectFilled(iconMin, iconMin + ImVec2(iconBox, iconBox), A(IM_COL32(0, 0, 0, 90)), Px(7.0f));
        DrawIcon(dl, look.icon, iconMin + ImVec2(iconBox, iconBox) * 0.5f, Px(19.0f), look.color);

        float textY = y + (height - content) * 0.5f;
        textY += Text(dl, fonts.light, titleSize, ImVec2(textX, textY), kText, item.title) + Px(3.0f);
        if (!item.detail.empty()) {
            Text(dl, fonts.regular, descSize, ImVec2(textX, textY), kText3, item.detail, textWidthFor(item));
        }

        const float trailing = trailingWidth(item);
        const float rowCenter = y + height * 0.5f;
        if (!item.actionLabel.empty()) {
            const ImVec2 buttonPos(listRight - Px(12.0f) - trailing, rowCenter - ButtonHeight(ButtonSize::Small) * 0.5f);
            ImGui::PushID(static_cast<int>(i));
            if (Button("##action", item.actionLabel, Icon::OpenExternal, buttonPos, ButtonSize::Small)) {
                OpenTarget(item.actionTarget);
            }
            ImGui::PopID();
        } else if (item.status == SetupItem::Status::Ok) {
            Chip(dl, ImVec2(listRight - Px(12.0f) - trailing, rowCenter - ChipHeight() * 0.5f), "Bereit", Tone::Ok);
        }
        y += height;
    }

    // Signatur automatisch erneuern (nur sinnvoll, wenn Sideloadly da ist).
    if (!m_setup.sideloadlyExe.empty()) {
        const char* title = "Signatur automatisch erneuern";
        std::string detail;
        switch (m_renew.GetState()) {
        case AutoRenew::State::Renewing:
            detail = "Läuft bald ab: Sideloadly erneuert sie gerade. Die App auf dem iPhone startet dabei kurz neu.";
            break;
        case AutoRenew::State::Off:
            detail = "Aus. Der Sideloadly-Dienst startet dann mit Windows und läuft dauerhaft im Hintergrund.";
            break;
        default:
            detail = "KrakznCam startet Sideloadly nur, wenn die Signatur in weniger als 2 Tagen abläuft.";
            if (m_renew.KnownExpiry() > 0) detail += " Gültig bis " + LongDate(m_renew.KnownExpiry()) + ".";
            break;
        }
        const float textWidth = listRight - Px(12.0f) - textX - SwitchWidth() - Px(16.0f);
        const float content =
            Measure(fonts.light, titleSize, title).y + Px(3.0f) + Measure(fonts.regular, descSize, detail, textWidth).y;
        const float height = std::max(iconBox, content) + Px(13.0f) * 2.0f;
        if (!items.empty()) {
            dl->AddRectFilled(ImVec2(listLeft + Px(12.0f), y), ImVec2(listRight - Px(12.0f), y + 1.0f),
                              A(White(0.07f)));
        }
        const bool enabled = m_renew.Enabled();
        const ImVec2 iconMin(listLeft + Px(12.0f), y + (height - iconBox) * 0.5f);
        dl->AddRectFilled(iconMin, iconMin + ImVec2(iconBox, iconBox), A(IM_COL32(0, 0, 0, 90)), Px(7.0f));
        DrawIcon(dl, Icon::Refresh, iconMin + ImVec2(iconBox, iconBox) * 0.5f, Px(19.0f), enabled ? kAccent : kText3);
        float textY = y + (height - content) * 0.5f;
        textY += Text(dl, fonts.light, titleSize, ImVec2(textX, textY), kText, title) + Px(3.0f);
        Text(dl, fonts.regular, descSize, ImVec2(textX, textY), kText3, detail, textWidth);
        bool value = enabled;
        if (Switch("##autorenew", ImVec2(listRight - Px(12.0f), y + height * 0.5f), value)) m_renew.SetEnabled(value);
        y += height;
    }

    // Fuß: erneut prüfen.
    y += Px(18.0f);
    const float buttonWidth = ButtonWidth("Erneut prüfen", Icon::Refresh, ButtonSize::Small);
    const float footTextWidth = width - padding.x * 2.0f - buttonWidth - Px(20.0f);
    const char* footText = "Wird auch automatisch geprüft, wenn du ins Fenster zurückkommst.";
    const float footTextHeight = Measure(fonts.regular, Px(13.0f), footText, footTextWidth).y;
    const float footHeight = std::max(ButtonHeight(ButtonSize::Small), footTextHeight);
    Text(dl, fonts.regular, Px(13.0f), ImVec2(pos.x + padding.x, y + (footHeight - footTextHeight) * 0.5f), kText3,
         footText, footTextWidth);
    const bool recheck = Button("##recheck", "Erneut prüfen", Icon::Refresh,
                                ImVec2(pos.x + width - padding.x - buttonWidth,
                                       y + (footHeight - ButtonHeight(ButtonSize::Small)) * 0.5f),
                                ButtonSize::Small);
    y += footHeight + padding.y;
    const float height = y - pos.y;

    dl->ChannelsSetCurrent(0);
    Card(dl, pos, pos + ImVec2(width, std::max(height, minHeight)), Px(16.0f), 0.78f);
    dl->ChannelsMerge();
    if (recheck) Recheck();  // erst nach dem Zeichnen: die Liste oben gehört noch zum alten Stand
    return height;
}

float App::DrawSideloadGuide(ImDrawList* dl, ImVec2 pos, float width, float minHeight) {
    const Fonts& fonts = GetFonts();
    const ImVec2 padding(Px(28.0f), Px(26.0f));
    const float x = pos.x + padding.x;
    const float inner = width - padding.x * 2.0f;
    const float circle = Px(26.0f);
    const float stepX = x + circle + Px(14.0f);
    const float stepWidth = inner - circle - Px(14.0f);
    const float stepSize = Px(14.5f);

    const std::wstring& package = m_setup.appPackage;
    std::string packageName = "KrakznCam.ipa";
    if (!package.empty()) packageName = Utf8(package.substr(package.find_last_of(L"\\/") + 1));

    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    float y = pos.y + padding.y;
    y += Kicker(dl, ImVec2(x, y), "SIDELOADLY", 11.0f) + Px(8.0f);
    y += Text(dl, fonts.light, Px(26.0f), ImVec2(x, y), kText, "App aufs iPhone bringen", inner) + Px(4.0f);
    y += Text(dl, fonts.light, Px(15.0f), ImVec2(x, y), kText2,
              "Einmal einrichten, danach erneuert Sideloadly die Signatur von selbst.", inner) +
         Px(20.0f);

    const std::string steps[] = {
        "iPhone per Kabel anschließen, entsperren und „Diesem Computer vertrauen“ bestätigen.",
        "Sideloadly öffnen und die Datei " + packageName + " in das Fenster ziehen.",
        "Apple-ID eintragen und auf Start klicken. Den Bestätigungscode gibst du direkt in Sideloadly ein.",
        "Auf dem iPhone: Einstellungen → Datenschutz & Sicherheit → Entwicklermodus einschalten. Das iPhone "
        "startet dafür neu.",
        "Einstellungen → Allgemein → VPN & Geräteverwaltung → deiner Apple-ID vertrauen. Dafür braucht das "
        "iPhone Internet.",
        "KrakznCam auf dem iPhone öffnen. Das Bild erscheint im Reiter Kamera.",
    };
    const float lineHeight = Measure(fonts.regular, stepSize, "A").y;
    for (size_t i = 0; i < std::size(steps); ++i) {
        const float textHeight = Measure(fonts.regular, stepSize, steps[i], stepWidth).y;
        const ImVec2 center(x + circle * 0.5f, y + circle * 0.5f);
        dl->AddCircleFilled(center, circle * 0.5f, A(White(0.10f)));
        const std::string number = std::to_string(i + 1);
        const ImVec2 numberSize = Measure(fonts.semibold, Px(13.0f), number);
        Text(dl, fonts.semibold, Px(13.0f), center - numberSize * 0.5f, kText, number);
        const float textTop = textHeight > lineHeight * 1.5f ? y : y + (circle - lineHeight) * 0.5f;
        Text(dl, fonts.regular, stepSize, ImVec2(stepX, textTop), kText2, steps[i], stepWidth);
        y += std::max(circle, textTop - y + textHeight);

        if (i == 1) {
            // Knöpfe direkt am passenden Schritt.
            y += Px(10.0f);
            float buttonX = stepX;
            auto place = [&](std::string_view label, char32_t icon) {
                const float buttonWidth = ButtonWidth(label, icon, ButtonSize::Small);
                if (buttonX > stepX && buttonX + buttonWidth > stepX + stepWidth) {
                    buttonX = stepX;
                    y += ButtonHeight(ButtonSize::Small) + Px(8.0f);
                }
                const ImVec2 at(buttonX, y);
                buttonX += buttonWidth + Px(10.0f);
                return at;
            };
            if (!m_setup.sideloadlyExe.empty()) {
                const ImVec2 at = place("Sideloadly öffnen", Icon::OpenExternal);
                if (Button("##guide_sideloadly", "Sideloadly öffnen", Icon::OpenExternal, at, ButtonSize::Small)) {
                    OpenTarget(m_setup.sideloadlyExe);
                }
            } else {
                const ImVec2 at = place("Sideloadly herunterladen", Icon::OpenExternal);
                if (Button("##guide_download", "Sideloadly herunterladen", Icon::OpenExternal, at, ButtonSize::Small)) {
                    OpenTarget(L"https://sideloadly.io");
                }
            }
            if (!package.empty()) {
                const ImVec2 at = place("App-Datei zeigen", Icon::Folder);
                if (Button("##guide_package", "App-Datei zeigen", Icon::Folder, at, ButtonSize::Small)) {
                    RevealInExplorer(package);
                }
                y += ButtonHeight(ButtonSize::Small);
            } else {
                y += ButtonHeight(ButtonSize::Small) + Px(6.0f);
                y += Text(dl, fonts.regular, Px(13.0f), ImVec2(stepX, y), kText3,
                          "Die App-Datei liegt dem KrakznCam-Download bei.", stepWidth);
            }
        }
        y += Px(14.0f);
    }

    // Tipp für den häufigsten Fehler beim ersten Mal.
    y += Px(2.0f);
    {
        const ImVec2 notePadding(Px(14.0f), Px(11.0f));
        const float iconSize = Px(15.0f);
        const float noteTextX = x + notePadding.x + iconSize + Px(10.0f);
        const float noteTextWidth = x + inner - notePadding.x - noteTextX;
        const char* tip = "Meldet Sideloadly „LOCKDOWN_E_INVALID_HOST_ID“: iPhone abstecken, wieder anstecken, "
                          "entsperren und erneut vertrauen.";
        const float noteTextHeight = Measure(fonts.regular, Px(13.5f), tip, noteTextWidth).y;
        const float noteHeight = noteTextHeight + notePadding.y * 2.0f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + inner, y + noteHeight), A(White(0.06f)), Px(9.0f));
        DrawIcon(dl, Icon::Info, ImVec2(x + notePadding.x + iconSize * 0.5f, y + notePadding.y + Px(9.0f)), iconSize,
                 kText2);
        Text(dl, fonts.regular, Px(13.5f), ImVec2(noteTextX, y + notePadding.y), kText2, tip, noteTextWidth);
        y += noteHeight;
    }
    y += padding.y;
    const float height = y - pos.y;

    dl->ChannelsSetCurrent(0);
    Card(dl, pos, pos + ImVec2(width, std::max(height, minHeight)), Px(16.0f), 0.78f);
    dl->ChannelsMerge();
    return height;
}
