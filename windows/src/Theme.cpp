#include "Theme.h"

#include "imgui_internal.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Theme {
namespace {

Fonts g_fonts;
float g_scale = 1.0f;
bool g_animating = false;
std::vector<float> g_alphaStack;
std::unordered_map<ImGuiID, float> g_values;

float CurrentAlpha() {
    return g_alphaStack.empty() ? 1.0f : g_alphaStack.back();
}

std::string FontPath(const char* file) {
    char windows[MAX_PATH];
    UINT length = GetWindowsDirectoryA(windows, MAX_PATH);
    std::string path = length ? std::string(windows, length) : std::string("C:\\Windows");
    return path + "\\Fonts\\" + file;
}

ImFont* LoadFirst(std::initializer_list<const char*> files) {
    for (const char* file : files) {
        const std::string path = FontPath(file);
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 16.0f);
        }
    }
    return nullptr;
}

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

size_t Utf8Length(unsigned char lead) {
    if (lead >= 0xF0) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC0) return 2;
    return 1;
}

// Pfad im Uhrzeigersinn um ein abgerundetes Rechteck: kurze Schritte in den Ecken,
// längere auf den Geraden (kurz genug, damit das Lichtband weich wandert).
void RoundedPath(std::vector<ImVec2>& out, ImVec2 min, ImVec2 max, float radius) {
    out.clear();
    radius = std::clamp(radius, 1.0f, std::min(max.x - min.x, max.y - min.y) * 0.5f);
    const ImVec2 centers[4] = {{max.x - radius, min.y + radius},
                               {max.x - radius, max.y - radius},
                               {min.x + radius, max.y - radius},
                               {min.x + radius, min.y + radius}};
    constexpr float quarter = IM_PI * 0.5f;
    auto add = [&out](ImVec2 p) {
        if (out.empty() || ImLengthSqr(p - out.back()) > 0.25f) out.push_back(p);
    };
    for (int corner = 0; corner < 4; ++corner) {
        const float start = -quarter + quarter * static_cast<float>(corner);
        const int steps = std::max(4, static_cast<int>(radius * quarter / 2.0f) + 1);
        for (int i = 0; i <= steps; ++i) {
            const float angle = start + quarter * static_cast<float>(i) / static_cast<float>(steps);
            add(centers[corner] + ImVec2(std::cos(angle), std::sin(angle)) * radius);
        }
        const ImVec2 from = out.back();
        const float end = start + quarter;
        const ImVec2 to = centers[(corner + 1) % 4] + ImVec2(std::cos(end), std::sin(end)) * radius;
        const int lineSteps = static_cast<int>(std::sqrt(ImLengthSqr(to - from)) / 6.0f);
        for (int i = 1; i < lineSteps; ++i) add(ImLerp(from, to, static_cast<float>(i) / static_cast<float>(lineSteps)));
    }
    while (out.size() > 2 && ImLengthSqr(out.back() - out.front()) <= 0.25f) out.pop_back();
}

// Band entlang eines geschlossenen Pfads, von Abstand a bis b (entlang der Normalen).
template <class ColorA, class ColorB>
void Band(ImDrawList* dl, const std::vector<ImVec2>& path, const std::vector<ImVec2>& normals, float a, float b,
          ColorA colorA, ColorB colorB) {
    const int n = static_cast<int>(path.size());
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(n * 6, (n + 1) * 2);
    const unsigned int base = dl->_VtxCurrentIdx;
    for (int j = 0; j <= n; ++j) {
        const int i = j % n;
        dl->PrimWriteVtx(path[i] + normals[i] * a, uv, colorA(i));
        dl->PrimWriteVtx(path[i] + normals[i] * b, uv, colorB(i));
    }
    for (int j = 0; j < n; ++j) {
        const unsigned int p0 = base + static_cast<unsigned int>(j) * 2;
        const unsigned int p1 = p0 + 2;
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0 + 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p1 + 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p1 + 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(p1));
    }
}

}  // namespace

// --- Farben ------------------------------------------------------------------------------

ImU32 White(float alpha) {
    return IM_COL32(255, 255, 255, static_cast<int>(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
}

ImU32 WithAlpha(ImU32 color, float alpha) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto channel = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<ImU32>(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) |
           channel(IM_COL32_A_SHIFT);
}

ImU32 A(ImU32 color) {
    return WithAlpha(color, CurrentAlpha());
}

void PushAlpha(float alpha) {
    g_alphaStack.push_back(CurrentAlpha() * std::clamp(alpha, 0.0f, 1.0f));
}

void PopAlpha() {
    if (!g_alphaStack.empty()) g_alphaStack.pop_back();
}

// --- Maße und Schriften ------------------------------------------------------------------

void SetScale(float scale) {
    g_scale = scale > 0.0f ? scale : 1.0f;
}

float Scale() {
    return g_scale;
}

bool LoadFonts() {
    g_fonts.regular = LoadFirst({"segoeui.ttf", "arial.ttf"});
    if (!g_fonts.regular) g_fonts.regular = ImGui::GetIO().Fonts->AddFontDefault();
    g_fonts.light = LoadFirst({"segoeuil.ttf", "segoeui.ttf"});
    g_fonts.semibold = LoadFirst({"seguisb.ttf", "segoeuib.ttf"});
    g_fonts.brand = LoadFirst({"bahnschrift.ttf"});
    g_fonts.icons = LoadFirst({"SegoeIcons.ttf", "segmdl2.ttf"});
    if (!g_fonts.light) g_fonts.light = g_fonts.regular;
    if (!g_fonts.semibold) g_fonts.semibold = g_fonts.regular;
    if (!g_fonts.brand) g_fonts.brand = g_fonts.semibold;
    ImGui::GetIO().FontDefault = g_fonts.regular;
    return true;
}

const Fonts& GetFonts() {
    return g_fonts;
}

// --- Text --------------------------------------------------------------------------------

ImVec2 Measure(ImFont* font, float size, std::string_view text, float wrap) {
    if (text.empty()) return ImVec2(0.0f, size);
    return font->CalcTextSizeA(size, FLT_MAX, wrap, text.data(), text.data() + text.size());
}

float Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, std::string_view text, float wrap) {
    if (text.empty()) return 0.0f;
    dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), A(color), text.data(),
                text.data() + text.size(), wrap);
    return Measure(font, size, text, wrap).y;
}

float SpacedWidth(ImFont* font, float size, std::string_view text, float spacing) {
    float width = 0.0f;
    int glyphs = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t n = Utf8Length(static_cast<unsigned char>(text[i]));
        width += Measure(font, size, text.substr(i, n)).x;
        i += n;
        ++glyphs;
    }
    return width + spacing * size * static_cast<float>(std::max(0, glyphs - 1));
}

void Spaced(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 color, std::string_view text,
            float spacing) {
    float x = pos.x;
    for (size_t i = 0; i < text.size();) {
        const size_t n = Utf8Length(static_cast<unsigned char>(text[i]));
        const std::string_view glyph = text.substr(i, n);
        Text(dl, font, size, ImVec2(x, pos.y), color, glyph);
        x += Measure(font, size, glyph).x + spacing * size;
        i += n;
    }
}

void DrawIcon(ImDrawList* dl, char32_t icon, ImVec2 center, float size, ImU32 color) {
    if (!g_fonts.icons) {
        dl->AddCircleFilled(center, size * 0.18f, A(color));
        return;
    }
    std::string glyph;
    AppendUtf8(glyph, icon);
    const ImVec2 measured = Measure(g_fonts.icons, size, glyph);
    dl->AddText(g_fonts.icons, size, ImVec2(std::floor(center.x - measured.x * 0.5f), std::floor(center.y - measured.y * 0.5f)),
                A(color), glyph.c_str());
}

// --- Flächen -----------------------------------------------------------------------------

void RadialGlow(ImDrawList* dl, ImVec2 center, ImVec2 radius, ImU32 color) {
    // Wie radial-gradient(… glow 0 %, transparent 70 %): Ringe mit weich abnehmender Deckkraft.
    constexpr int segments = 72;
    const float stops[] = {0.0f, 0.18f, 0.38f, 0.58f, 0.78f, 1.0f};
    const float alphas[] = {1.0f, 0.86f, 0.6f, 0.32f, 0.11f, 0.0f};
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    for (int ring = 0; ring + 1 < static_cast<int>(std::size(stops)); ++ring) {
        const ImU32 inner = A(WithAlpha(color, alphas[ring]));
        const ImU32 outer = A(WithAlpha(color, alphas[ring + 1]));
        dl->PrimReserve(segments * 6, (segments + 1) * 2);
        const unsigned int base = dl->_VtxCurrentIdx;
        for (int i = 0; i <= segments; ++i) {
            const float angle = 2.0f * IM_PI * static_cast<float>(i) / segments;
            const ImVec2 direction(std::cos(angle) * radius.x, std::sin(angle) * radius.y);
            dl->PrimWriteVtx(center + direction * stops[ring], uv, inner);
            dl->PrimWriteVtx(center + direction * stops[ring + 1], uv, outer);
        }
        for (int i = 0; i < segments; ++i) {
            const unsigned int p0 = base + static_cast<unsigned int>(i) * 2;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0 + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0 + 3));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0 + 3));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(p0 + 2));
        }
    }
}

void Backdrop(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const float height = max.y - min.y;
    const float split = min.y + height * 0.7f;
    dl->AddRectFilledMultiColor(min, ImVec2(max.x, split), kBgHi, kBgHi, kBg, kBg);
    dl->AddRectFilled(ImVec2(min.x, split), max, kBg);
    // Lichtschein: 90 % × 70 % groß, Mitte 18 % über der Oberkante, verblasst bei 70 %.
    RadialGlow(dl, ImVec2((min.x + max.x) * 0.5f, min.y - height * 0.18f),
               ImVec2((max.x - min.x) * 0.9f * 0.7f, height * 0.7f * 0.7f), kGlow);
}

void Shadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float spread, float alpha) {
    if (alpha <= 0.0f) return;
    constexpr int layers = 10;
    const ImVec2 offset(0.0f, spread * 0.33f);
    for (int i = layers; i >= 1; --i) {
        const float t = static_cast<float>(i) / layers;
        const float grow = spread * t;
        const float a = alpha * (1.0f - t) * (1.0f - t) * 2.2f / layers;
        dl->AddRectFilled(min - ImVec2(grow, grow) + offset, max + ImVec2(grow, grow) + offset,
                          A(IM_COL32(0, 0, 0, static_cast<int>(255.0f * std::min(a, 1.0f)))), rounding + grow);
    }
}

void Card(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float opacity) {
    Shadow(dl, min, max, rounding, Px(30.0f), 0.5f);
    dl->AddRectFilled(min, max, A(WithAlpha(kPanel, opacity)), rounding);
}

void LiveRing(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
    const float width = std::max(1.5f, Px(2.4f));
    const float gap = std::max(2.0f, Px(4.0f));
    const float out = gap + width * 0.5f;
    static std::vector<ImVec2> path;
    static std::vector<ImVec2> normals;
    static std::vector<float> light;
    RoundedPath(path, min - ImVec2(out, out), max + ImVec2(out, out), rounding + out);
    const int n = static_cast<int>(path.size());
    if (n < 3) return;
    normals.resize(path.size());
    light.resize(path.size());
    const ImVec2 center = (min + max) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const ImVec2 tangent = path[(i + 1) % n] - path[(i + n - 1) % n];
        const float length = std::max(std::sqrt(ImLengthSqr(tangent)), 0.0001f);
        normals[i] = ImVec2(tangent.y / length, -tangent.x / length);
    }
    // Ein Lichtband, das alle 3,2 s einmal herumläuft.
    const float spin = Animations() ? static_cast<float>(std::fmod(ImGui::GetTime() / 3.2, 1.0)) : 0.1f;
    for (int i = 0; i < n; ++i) {
        float t = std::atan2(path[i].x - center.x, center.y - path[i].y) / (2.0f * IM_PI) - spin;
        t -= std::floor(t);
        light[i] = (t >= 0.5f && t < 0.72f) ? (t - 0.5f) / 0.22f : (t >= 0.72f && t < 0.94f) ? (0.94f - t) / 0.22f : 0.0f;
    }

    const float half = width * 0.5f;
    const float sigma = std::max(1.5f, Px(2.7f));
    auto glow = [&](float strength) {
        return [strength](int i) {
            return A(IM_COL32(196, 214, 255, static_cast<int>(255.0f * 0.5f * strength * (0.5f + 0.5f * light[i]))));
        };
    };
    auto ring = [](float opacity) {
        return [opacity](int i) {
            const float k = light[i];
            auto mix = [k](float a, float b) { return static_cast<int>(a + (b - a) * k); };
            return A(IM_COL32(mix(226, 255), mix(233, 255), mix(247, 255),
                              static_cast<int>(255.0f * opacity * (0.55f + 0.45f * k))));
        };
    };
    // Weicher Schein nach innen und außen, dann der Ring selbst mit einem Pixel Kantenglättung.
    const float steps[] = {0.0f, 1.0f, 2.0f, 3.0f};
    const float strength[] = {1.0f, 0.45f, 0.12f, 0.0f};
    for (int side = -1; side <= 1; side += 2) {
        for (int s = 0; s < 3; ++s) {
            Band(dl, path, normals, side * (half + steps[s] * sigma), side * (half + steps[s + 1] * sigma),
                 glow(strength[s]), glow(strength[s + 1]));
        }
    }
    const float core = std::max(half - 0.5f, 0.0f);
    Band(dl, path, normals, -half - 0.5f, -core, ring(0.0f), ring(1.0f));
    if (core > 0.0f) Band(dl, path, normals, -core, core, ring(1.0f), ring(1.0f));
    Band(dl, path, normals, core, half + 0.5f, ring(1.0f), ring(0.0f));
}

void Meter(ImDrawList* dl, ImVec2 min, ImVec2 max, float fraction) {
    const float radius = (max.y - min.y) * 0.5f;
    dl->AddRectFilled(min, max, A(White(0.10f)), radius);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0.0f) return;
    const ImVec2 end(min.x + std::max((max.x - min.x) * fraction, radius * 2.0f), max.y);
    dl->AddRectFilled(min, end, A(kAccent), radius);
}

// --- Bewegung ----------------------------------------------------------------------------

bool Animations() {
    static const bool enabled = [] {
        BOOL value = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &value, 0) || value;
    }();
    return enabled;
}

float Ease(float t) {
    // Näherung an Eidolons --ease (cubic-bezier(.22, .61, .36, 1)).
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

float Animate(ImGuiID id, float target, float speed) {
    auto [it, inserted] = g_values.try_emplace(id, target);
    float& value = it->second;
    if (inserted || !Animations()) {
        value = target;
        return value;
    }
    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
    value += (target - value) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(target - value) < 0.003f) value = target;
    else g_animating = true;
    return value;
}

void BeginFrame() {
    g_animating = false;
    g_alphaStack.clear();
}

bool Animating() {
    return g_animating;
}

// --- Bedienelemente ----------------------------------------------------------------------

float ButtonHeight(ButtonSize size) {
    return Px(size == ButtonSize::Small ? 34.0f : 44.0f);
}

namespace {
float ButtonFontSize(ButtonSize size) {
    return Px(size == ButtonSize::Small ? 13.5f : 15.0f);
}
float ButtonPadding(ButtonSize size) {
    return Px(size == ButtonSize::Small ? 15.0f : 22.0f);
}
}  // namespace

float ButtonWidth(std::string_view label, char32_t icon, ButtonSize size) {
    const float font = ButtonFontSize(size);
    const float iconWidth = icon ? font * 1.15f + Px(8.0f) : 0.0f;
    return ButtonPadding(size) * 2.0f + iconWidth + Measure(g_fonts.semibold, font, label).x;
}

bool Button(const char* id, std::string_view label, char32_t icon, ImVec2 pos, ButtonSize size) {
    const ImVec2 box(ButtonWidth(label, icon, size), ButtonHeight(size));
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, box);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImGuiID key = ImGui::GetItemID();
    const float hover = Animate(key, hovered ? 1.0f : 0.0f, 18.0f);
    const float press = Animate(key ^ 0x5bd1e995u, held ? 1.0f : 0.0f, 26.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = 1.0f + 0.05f * hover - 0.035f * press;
    const ImVec2 center = pos + box * 0.5f;
    const ImVec2 half = box * 0.5f * scale;
    const float radius = half.y;
    if (hover > 0.01f) {
        // Wie Eidolons Fokus: weißer Knopf, heller Saum, Schatten darunter.
        Shadow(dl, center - half, center + half, radius, Px(16.0f), 0.55f * hover);
        const float ring = Px(3.0f);
        dl->AddRectFilled(center - half - ImVec2(ring, ring), center + half + ImVec2(ring, ring),
                          A(White(0.16f * hover)), radius + ring);
    }
    dl->AddRectFilled(center - half, center + half, A(Mix(White(0.14f), IM_COL32_WHITE, hover)), radius);

    const float font = ButtonFontSize(size) * scale;
    const ImU32 color = Mix(kText, kBg, hover);
    const ImVec2 textSize = Measure(g_fonts.semibold, font, label);
    const float iconSize = icon ? font * 1.15f : 0.0f;
    const float gap = icon ? Px(8.0f) * scale : 0.0f;
    float x = center.x - (iconSize + gap + textSize.x) * 0.5f;
    if (icon) {
        DrawIcon(dl, icon, ImVec2(x + iconSize * 0.5f, center.y), iconSize, color);
        x += iconSize + gap;
    }
    Text(dl, g_fonts.semibold, font, ImVec2(x, center.y - textSize.y * 0.5f), color, label);
    return clicked;
}

bool Tab(const char* id, std::string_view label, ImVec2 pos, bool active, bool badge, float& width) {
    ImFont* font = active ? g_fonts.semibold : g_fonts.regular;
    const float size = Px(18.0f);
    const ImVec2 padding(Px(14.0f), Px(6.0f));
    const ImVec2 text = Measure(font, size, label);
    const ImVec2 box(text.x + padding.x * 2.0f, text.y + padding.y * 2.0f);
    width = box.x;
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, box);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hover = Animate(ImGui::GetItemID(), hovered && !active ? 1.0f : 0.0f, 18.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hover > 0.01f) dl->AddRectFilled(pos, pos + box, A(White(0.10f * hover)), box.y * 0.5f);
    const ImU32 color = active ? kText : Mix(kText3, kText2, hover);
    Text(dl, font, size, pos + padding, color, label);
    if (badge) {
        dl->AddCircleFilled(ImVec2(pos.x + padding.x + text.x + Px(5.0f), pos.y + padding.y + Px(4.0f)), Px(3.5f),
                            A(kRed));
    }
    return clicked;
}

namespace {
struct ToneColors {
    ImU32 background;
    ImU32 foreground;
};
ToneColors ColorsFor(Tone tone) {
    switch (tone) {
    case Tone::Ok: return {WithAlpha(kGreen, 0.16f), kGreen};
    case Tone::Warn: return {WithAlpha(kGold, 0.14f), kGold};
    case Tone::Bad: return {WithAlpha(kRed, 0.14f), kRed};
    case Tone::Solid: return {WithAlpha(kText, 0.88f), kBg};
    case Tone::Neutral: break;
    }
    return {White(0.10f), kText2};
}
}  // namespace

float ChipHeight() {
    return Px(28.0f);
}

float ChipWidth(std::string_view label, bool dot) {
    const float font = Px(13.0f);
    return Px(11.0f) * 2.0f + (dot ? Px(14.0f) : 0.0f) + Measure(g_fonts.semibold, font, label).x;
}

float Chip(ImDrawList* dl, ImVec2 pos, std::string_view label, Tone tone, bool dot) {
    const ToneColors colors = ColorsFor(tone);
    const float font = Px(13.0f);
    const float width = ChipWidth(label, dot);
    const float height = ChipHeight();
    dl->AddRectFilled(pos, pos + ImVec2(width, height), A(colors.background), height * 0.5f);
    float x = pos.x + Px(11.0f);
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + Px(4.0f), pos.y + height * 0.5f), Px(4.0f), A(colors.foreground));
        x += Px(14.0f);
    }
    const ImVec2 text = Measure(g_fonts.semibold, font, label);
    Text(dl, g_fonts.semibold, font, ImVec2(x, pos.y + (height - text.y) * 0.5f), colors.foreground, label);
    return width;
}

}  // namespace Theme
