// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Skin.h"

#include <cstdio>

namespace rabbitears {

// ---- built-in skins ---------------------------------------------------------
// Dark reproduces makeDarkTheme() (Win32/ui/Theme.h) exactly. Light reproduces
// makeLightTheme() but with PORTABLE literals in place of its three GetSysColor()
// lookups (windowBg/panelBg -> white, textPrimary -> black).
//
// RATIFIED DESIGN (THEME_ENGINE.md §4): a theme engine's skins DEFINE their own
// colours — they do not inherit the OS window/text colours. The pre-engine light
// theme used GetSysColor for convenience; the engine supersedes that. "Follow
// system" (renderer-side) only chooses the dark-vs-light SKIN by the OS mode; it
// never sources colours from the OS. So the Win32 resolver must NOT call GetSysColor,
// and the mac renderer mirrors these literals. Fonts mirror the Win32 ad-hoc fonts
// (14/16/13 px @96dpi -> pt); the glyph role is the platform symbol font.

Skin makeDarkSkin() {
    Skin s;
    s.id = "dark";
    s.name = "Dark";
    s.dark = true;
    s.palette = SkinPalette{
        .windowBg = {22, 22, 24},   .panelBg = {26, 26, 28},   .panelElevBg = {32, 32, 35},
        .altRowBg = {28, 28, 31},   .hoverBg = {42, 42, 45},   .border = {48, 48, 52},
        .textPrimary = {230, 230, 232}, .textSecondary = {154, 154, 160}, .textMuted = {106, 106, 112},
        .accent = {217, 119, 87},   .accentText = {40, 18, 10}, .selectionBg = {92, 52, 38},
        .selectionText = {247, 238, 233}, .dangerHover = {196, 43, 28},
    };
    s.body = {"Segoe UI", 10.5f, 400};
    s.title = {"Segoe UI", 12.0f, 600};
    s.glyph = {"Segoe MDL2 Assets", 9.75f, 400, /*symbol=*/true};
    return s;
}

Skin makeLightSkin() {
    Skin s;
    s.id = "light";
    s.name = "Light";
    s.dark = false;
    s.palette = SkinPalette{
        .windowBg = {255, 255, 255}, .panelBg = {255, 255, 255}, .panelElevBg = {244, 244, 246},
        .altRowBg = {245, 245, 245}, .hoverBg = {232, 232, 234}, .border = {214, 214, 218},
        .textPrimary = {0, 0, 0},   .textSecondary = {96, 96, 102}, .textMuted = {140, 140, 146},
        .accent = {193, 95, 60},    .accentText = {255, 255, 255}, .selectionBg = {250, 232, 224},
        .selectionText = {74, 27, 12}, .dangerHover = {196, 43, 28},
    };
    s.body = {"Segoe UI", 10.5f, 400};
    s.title = {"Segoe UI", 12.0f, 600};
    s.glyph = {"Segoe MDL2 Assets", 9.75f, 400, /*symbol=*/true};
    return s;
}

const std::vector<Skin>& builtinSkins() {
    static const std::vector<Skin> skins = {makeDarkSkin(), makeLightSkin()};
    return skins;
}

const Skin& skinById(const std::string& id) {
    for (const Skin& s : builtinSkins())
        if (s.id == id) return s;
    return builtinSkins().front();  // unknown -> dark
}

std::vector<std::string> builtinSkinIds() {
    std::vector<std::string> ids;
    for (const Skin& s : builtinSkins()) ids.push_back(s.id);
    return ids;
}

const char* defaultSkinId() { return "dark"; }

const char* skinSettingKey() { return "skin"; }

// ---- (de)serialization ------------------------------------------------------
namespace {

std::string hex2(uint8_t v) {
    char b[3];
    std::snprintf(b, sizeof b, "%02X", v);
    return b;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Parse the two hex chars at [pos] into `out`; false on any non-hex digit.
bool hexByte(const std::string& s, size_t pos, uint8_t& out) {
    const int hi = nibble(s[pos]);
    const int lo = nibble(s[pos + 1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

}  // namespace

std::string skinColorToString(const SkinColor& c) {
    if (c.inherit) return "inherit";
    std::string s = hex2(c.r) + hex2(c.g) + hex2(c.b);
    if (c.a != 255) s += hex2(c.a);  // only emit alpha when not fully opaque
    return s;
}

SkinColor skinColorFromString(const std::string& s, const SkinColor& fallback) {
    if (s == "inherit") {
        SkinColor c{};
        c.inherit = true;
        return c;
    }
    if (s.size() != 6 && s.size() != 8) return fallback;
    SkinColor c{};
    if (!hexByte(s, 0, c.r) || !hexByte(s, 2, c.g) || !hexByte(s, 4, c.b)) return fallback;
    c.a = 255;
    if (s.size() == 8 && !hexByte(s, 6, c.a)) return fallback;
    c.inherit = false;
    return c;
}

std::string skinPaletteToString(const SkinPalette& p) {
    const SkinColor roles[14] = {
        p.windowBg,   p.panelBg,       p.panelElevBg, p.altRowBg,      p.hoverBg,     p.border,
        p.textPrimary, p.textSecondary, p.textMuted,
        p.accent,     p.accentText,    p.selectionBg, p.selectionText, p.dangerHover};
    std::string out;
    for (int i = 0; i < 14; ++i) {
        if (i) out += ',';
        out += skinColorToString(roles[i]);
    }
    return out;
}

SkinPalette skinPaletteFromString(const std::string& s, const SkinPalette& fallback) {
    std::vector<std::string> tok;
    size_t start = 0;
    for (;;) {
        const size_t comma = s.find(',', start);
        if (comma == std::string::npos) {
            tok.push_back(s.substr(start));
            break;
        }
        tok.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    if (tok.size() != 14) return fallback;  // exact arity or whole fallback (meter discipline)

    const SkinColor fb[14] = {
        fallback.windowBg,   fallback.panelBg,       fallback.panelElevBg, fallback.altRowBg,
        fallback.hoverBg,    fallback.border,        fallback.textPrimary, fallback.textSecondary,
        fallback.textMuted,  fallback.accent,        fallback.accentText,  fallback.selectionBg,
        fallback.selectionText, fallback.dangerHover};
    SkinColor r[14];
    for (int i = 0; i < 14; ++i) r[i] = skinColorFromString(tok[i], fb[i]);

    SkinPalette p;
    p.windowBg = r[0];      p.panelBg = r[1];       p.panelElevBg = r[2];  p.altRowBg = r[3];
    p.hoverBg = r[4];       p.border = r[5];        p.textPrimary = r[6];  p.textSecondary = r[7];
    p.textMuted = r[8];     p.accent = r[9];        p.accentText = r[10];  p.selectionBg = r[11];
    p.selectionText = r[12]; p.dangerHover = r[13];
    return p;
}

}  // namespace rabbitears
