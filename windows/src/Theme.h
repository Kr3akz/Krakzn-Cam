#pragma once
// KrakznCams Formensprache, angelehnt an Eidolon: Flächen trennen sich durch Helligkeit
// statt durch Linien, Tiefe entsteht durch Schatten. Farben neutral – Schwarz, Grautöne,
// Weiß für Fokus und Hover, Blau nur für Fortschritt.

#include "imgui.h"

#include <string>
#include <string_view>

namespace Theme {

// --- Farben -----------------------------------------------------------------------------
constexpr ImU32 kBg = IM_COL32(11, 11, 13, 255);
constexpr ImU32 kBgHi = IM_COL32(22, 22, 25, 255);
constexpr ImU32 kGlow = IM_COL32(46, 46, 52, 255);
constexpr ImU32 kPanel = IM_COL32(31, 31, 34, 255);
constexpr ImU32 kPanelHi = IM_COL32(42, 42, 46, 255);
constexpr ImU32 kText = IM_COL32(244, 244, 246, 255);
constexpr ImU32 kText2 = IM_COL32(172, 172, 178, 255);
constexpr ImU32 kText3 = IM_COL32(112, 112, 118, 255);
constexpr ImU32 kAccent = IM_COL32(46, 125, 246, 255);
constexpr ImU32 kGreen = IM_COL32(96, 214, 144, 255);
constexpr ImU32 kRed = IM_COL32(255, 110, 110, 255);
constexpr ImU32 kGold = IM_COL32(240, 190, 90, 255);

ImU32 White(float alpha);
ImU32 WithAlpha(ImU32 color, float alpha);  // multipliziert die vorhandene Deckkraft
ImU32 Mix(ImU32 a, ImU32 b, float t);
// Wendet die aktuelle Seiten-Deckkraft an (siehe PushAlpha).
ImU32 A(ImU32 color);

// Deckkraft für alles, was danach gezeichnet wird (Einblenden einer Seite).
void PushAlpha(float alpha);
void PopAlpha();

// --- Maße ---------------------------------------------------------------------------------
void SetScale(float scale);
float Scale();
inline float Px(float value) { return value * Scale(); }

// --- Schriften (aus Windows) --------------------------------------------------------------
struct Fonts {
    ImFont* regular = nullptr;   // Segoe UI
    ImFont* light = nullptr;     // Segoe UI Light: Titel und Kennzahlen
    ImFont* semibold = nullptr;  // Segoe UI Semibold: Knöpfe und Hervorhebungen
    ImFont* brand = nullptr;     // Bahnschrift: kleine Großbuchstaben-Beschriftungen
    ImFont* icons = nullptr;     // Segoe Fluent Icons (Windows 11) bzw. MDL2 Assets
};
bool LoadFonts();
const Fonts& GetFonts();

// Symbole aus Segoe Fluent Icons / MDL2 Assets.
namespace Icon {
constexpr char32_t Check = 0xE73E;
constexpr char32_t Cancel = 0xE711;
constexpr char32_t Warning = 0xE7BA;
constexpr char32_t Info = 0xE946;
constexpr char32_t Refresh = 0xE72C;
constexpr char32_t Download = 0xE896;
constexpr char32_t OpenExternal = 0xE8A7;
constexpr char32_t Video = 0xE714;
constexpr char32_t Phone = 0xE8EA;
constexpr char32_t Settings = 0xE713;
constexpr char32_t Calendar = 0xE787;
constexpr char32_t Monitor = 0xE7F4;
constexpr char32_t Usb = 0xE88E;
constexpr char32_t Camera = 0xE722;
constexpr char32_t Folder = 0xE8B7;
}  // namespace Icon

// --- Text ---------------------------------------------------------------------------------
ImVec2 Measure(ImFont* font, float size, std::string_view text, float wrap = 0.0f);
// Zeichnet Text und gibt seine Höhe zurück.
float Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, std::string_view text, float wrap = 0.0f);
// Gesperrte Großbuchstaben wie Eidolons Beschriftungen (spacing in em).
float SpacedWidth(ImFont* font, float size, std::string_view text, float spacing);
void Spaced(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, std::string_view text, float spacing);
void DrawIcon(ImDrawList* dl, char32_t icon, ImVec2 center, float size, ImU32 color);

// --- Flächen ------------------------------------------------------------------------------
// Hintergrund: Verlauf von oben nach unten mit sanftem Lichtschein oben in der Mitte.
void Backdrop(ImDrawList* dl, ImVec2 min, ImVec2 max);
void RadialGlow(ImDrawList* dl, ImVec2 center, ImVec2 radius, ImU32 color);
void Shadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float alpha);
// Karte: halbtransparente Fläche mit weichem Schatten.
void Card(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float opacity = 0.62f);
// Leuchtender Ring mit wanderndem Lichtband, wie Eidolons Fokus-Ring.
void LiveRing(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding);
// Fortschrittsbalken (blau).
void Meter(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction);

// --- Bewegung -----------------------------------------------------------------------------
bool Animations();  // false, wenn Windows weniger Animationen zeigen soll
float Ease(float t);
// Nähert einen Wert weich an sein Ziel an; solange er unterwegs ist, gilt Animating().
float Animate(ImGuiID id, float target, float speed = 14.0f);
void BeginFrame();
bool Animating();

// --- Bedienelemente (absolut positioniert) ------------------------------------------------
enum class ButtonSize { Normal, Small };
float ButtonWidth(std::string_view label, char32_t icon, ButtonSize size);
float ButtonHeight(ButtonSize size);
// Pillen-Knopf: halbtransparent, beim Darüberfahren weiß mit dunkler Schrift.
bool Button(const char* id, std::string_view label, char32_t icon, ImVec2 pos, ButtonSize size = ButtonSize::Normal);

// Reiter in der Kopfzeile. Gibt true zurück, wenn er angeklickt wurde; width = belegte Breite.
bool Tab(const char* id, std::string_view label, ImVec2 pos, bool active, bool badge, float& width);

// Schalter wie in Eidolon: Spur grau bzw. blau, weißer Knopf. Gibt true zurück, wenn umgeschaltet.
float SwitchWidth();
bool Switch(const char* id, ImVec2 rightCenter, bool& value);

enum class Tone { Neutral, Ok, Warn, Bad, Solid };
float ChipWidth(std::string_view label, bool dot);
// Kleine Pille mit Text; dot zeigt einen farbigen Punkt davor. Gibt die Breite zurück.
float Chip(ImDrawList* dl, ImVec2 pos, std::string_view label, Tone tone, bool dot = false);
float ChipHeight();

}  // namespace Theme
