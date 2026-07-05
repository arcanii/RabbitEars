// SPDX-License-Identifier: GPL-3.0-or-later
#include "MeterModel.h"

#include <cmath>    // std::isfinite — reject NaN/inf in meterTuningFromString
#include <cstdio>   // std::snprintf
#include <cstdlib>  // std::strtof
#include <string>
#include <vector>

namespace rabbitears::mac {

namespace {

// Split `s` on commas into its fields (mirrors Skin.cpp). An empty string yields one
// empty token; N commas yield N+1 tokens.
std::vector<std::string> splitCommas(const std::string& s) {
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
    return tok;
}

// A meter-palette colour token: like skinColorFromString, but also accepts the legacy
// Win32 "theme" spelling as the inherit sentinel (skinColorFromString knows only
// "inherit"). Forward-compat for when Win32 migrates and reads its old persisted bg.
SkinColor meterColorFromToken(const std::string& tok, const SkinColor& fallback) {
    if (tok == "theme") {
        SkinColor c{};
        c.inherit = true;
        return c;
    }
    return skinColorFromString(tok, fallback);
}

}  // namespace

std::string meterStyleToString(MeterStyle s) {
    switch (s) {
        case MeterStyle::Led:   return "led";
        case MeterStyle::Tube:  return "tube";
        case MeterStyle::Lcd:   return "lcd";
        case MeterStyle::Scope: return "scope";
    }
    return "led";
}

MeterStyle meterStyleFromString(const std::string& s, MeterStyle fallback) {
    if (s == "led")   return MeterStyle::Led;
    if (s == "tube")  return MeterStyle::Tube;
    if (s == "lcd")   return MeterStyle::Lcd;
    if (s == "scope") return MeterStyle::Scope;
    return fallback;
}

std::string meterPaletteToString(const MeterPalette& p) {
    const SkinColor roles[7] = {p.bg, p.off, p.low, p.mid, p.high, p.accent, p.peak};
    std::string out;
    for (int i = 0; i < 7; ++i) {
        if (i) out += ',';
        out += skinColorToString(roles[i]);
    }
    return out;
}

MeterPalette meterPaletteFromString(const std::string& s, const MeterPalette& fallback) {
    const std::vector<std::string> tok = splitCommas(s);
    if (tok.size() != 7) return fallback;  // exact arity or whole fallback
    const SkinColor fb[7] = {fallback.bg,   fallback.off,    fallback.low, fallback.mid,
                             fallback.high,  fallback.accent, fallback.peak};
    SkinColor r[7];
    for (int i = 0; i < 7; ++i) r[i] = meterColorFromToken(tok[i], fb[i]);
    return MeterPalette{r[0], r[1], r[2], r[3], r[4], r[5], r[6]};
}

// NB: %.3f / strtof assume the C locale's '.' decimal point (which the app keeps — it
// never calls setlocale), same invariant as skinGpuToString. See Skin.cpp.
std::string meterTuningToString(const MeterTuning& t) {
    char b[64];
    std::snprintf(b, sizeof b, "%.3f,%.3f,%.3f,%.3f,%.3f", t.glow, t.smoothing,
                  t.sensitivity, t.peakHold, t.breathing);
    return b;
}

MeterTuning meterTuningFromString(const std::string& s, const MeterTuning& fallback) {
    const std::vector<std::string> tok = splitCommas(s);
    if (tok.size() != 5) return fallback;  // exact arity or whole fallback
    // Parse one 0..1 float; on junk / NaN / inf keep this field's fallback (per-field
    // fallback within a valid token count — the "meter discipline").
    auto parse01 = [](const std::string& t, float fb) -> float {
        char* end = nullptr;
        const float v = std::strtof(t.c_str(), &end);
        if (end == t.c_str() || *end != '\0' || !std::isfinite(v)) return fb;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);  // clamp 0..1
    };
    MeterTuning r;
    r.glow        = parse01(tok[0], fallback.glow);
    r.smoothing   = parse01(tok[1], fallback.smoothing);
    r.sensitivity = parse01(tok[2], fallback.sensitivity);
    r.peakHold    = parse01(tok[3], fallback.peakHold);
    r.breathing   = parse01(tok[4], fallback.breathing);
    return r;
}

}  // namespace rabbitears::mac
