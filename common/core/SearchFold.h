// SPDX-License-Identifier: GPL-3.0-or-later
//
// searchFold — one character folded for "does this match what was typed" comparisons made outside
// SQLite: marking the matched text in a programme-search result (common/db/Database.cpp), and the TV
// guide's channel-name matching (Win32/ui/EpgGuideControl.cpp). Lower-cases ASCII, Latin-1 (and µ),
// Latin Extended-A, Greek (tonos kept, final ς → σ) and basic Cyrillic (and Ukrainian Ґ), and drops a
// Latin letter's accents (É → e, ç → c, Romanian ș → s) — what the FTS5 tokenizers' case folding and
// remove_diacritics do for those ranges, so a title the index found for "quebec" can have its "Québec"
// marked. Everything else is returned unchanged. Known differences from FTS5: the stroke letters
// Ø Đ Ħ Ŀ Ł Ŧ fold to their base letter here ("lodz" finds the channel "TVP3 Łódź"; FTS5 keeps them,
// so a programme search for "lodz" does not find "Łódź"), and the accents FTS5 strips beyond these
// ranges (U+01A0–U+0233 bar Ș Ț — Vietnamese, pinyin, … — and U+1E00–U+1EF9, e.g. Welsh ẃ, Ḥ) are kept
// here: found by the index, left unmarked. (RecordingRules.cpp's foldChar is a different, case-only
// fold — the same case mappings bar µ and Ґ, but accents kept (Ș → ș, not s): a recording rule's
// title match is deliberately stricter than "find what I typed".)
//
// One character in, one out, so a position in the folded text is the same position in the original
// (the callers compare character by character). Precomposed characters only: a letter followed by a
// COMBINING accent keeps that accent as a character of its own (XMLTV text is NFC in practice).
//
// Header-only + inline (like FeatureFlags.h): any translation unit on either platform can use it with
// no build-file change. wchar_t is 16-bit on Windows and 32-bit on macOS; every range here is BMP.
#pragma once

namespace rabbitears {

inline wchar_t searchFold(wchar_t c) {
    const unsigned long u = static_cast<unsigned long>(c);
    if (u < 0x80) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    if (u == 0xB5) return static_cast<wchar_t>(0x3BC);  // µ (micro sign) → μ, as FTS5 folds it
    if (u >= 0xC0 && u <= 0x17F) {
        // U+00C0–U+017F, one entry per code point: the base letter, or '*' = keep the character but
        // lower-case it when it is a capital (Æ Ð Þ Ĳ Ŋ Œ — ligatures and letters of their own), or
        // '.' = keep it as it is (× ÷ ß ĸ ŉ ı).
        static const char kBase[] =
            "aaaaaa*ceeeeiiii*nooooo.ouuuuy*."   // U+00C0–U+00DF
            "aaaaaa*ceeeeiiii*nooooo.ouuuuy*y"   // U+00E0–U+00FF
            "aaaaaaccccccccddddeeeeeeeeeegggg"   // U+0100–U+011F
            "gggghhhhiiiiiiiii.**jjkk.llllllll"  // U+0120–U+0140 (33: ı at U+0131, ĸ at U+0138)
            "llnnnnnn.**oooooo**rrrrrrssssssss"  // U+0141–U+0161
            "ttttttuuuuuuuuuuuuwwyyyzzzzzzs";    // U+0162–U+017F
        static_assert(sizeof(kBase) - 1 == 0x17F - 0xC0 + 1, "one entry per code point");
        const char b = kBase[u - 0xC0];
        if (b == '.') return c;
        if (b != '*') return static_cast<wchar_t>(b);
        if (u <= 0xDE) return static_cast<wchar_t>(u + 0x20);  // Æ Ð Þ → æ ð þ
        if (u >= 0x100 && (u & 1) == 0) return static_cast<wchar_t>(u + 1);  // Ĳ Ŋ Œ → ĳ ŋ œ
        return c;
    }
    if (u >= 0x218 && u <= 0x21B) return (u < 0x21A) ? L's' : L't';  // Romanian Ș ș Ț ț (comma below)
    // Greek: case only — the tokenizers keep the tonos (Ά → ά, not α) — and final ς as σ.
    if (u >= 0x391 && u <= 0x3AB && u != 0x3A2) return static_cast<wchar_t>(u + 0x20);  // Α–Ω, Ϊ Ϋ
    if (u == 0x386) return static_cast<wchar_t>(0x3AC);                                  // Ά
    if (u >= 0x388 && u <= 0x38A) return static_cast<wchar_t>(u + 0x25);                 // Έ Ή Ί
    if (u == 0x38C) return static_cast<wchar_t>(0x3CC);                                  // Ό
    if (u == 0x38E || u == 0x38F) return static_cast<wchar_t>(u + 0x3F);                 // Ύ Ώ
    if (u == 0x3C2) return static_cast<wchar_t>(0x3C3);                                  // ς
    if (u >= 0x410 && u <= 0x42F) return static_cast<wchar_t>(u + 0x20);                // Cyrillic А–Я
    if (u >= 0x400 && u <= 0x40F) return static_cast<wchar_t>(u + 0x50);                // Cyrillic Ѐ–Џ
    if (u == 0x490) return static_cast<wchar_t>(0x491);                                  // Ukrainian Ґ
    return c;
}

}  // namespace rabbitears
