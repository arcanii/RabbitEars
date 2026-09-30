// SPDX-License-Identifier: GPL-3.0-or-later
//
// Character folds for "does this match what was typed" comparisons made outside SQLite.
//
// ftsFold — EXACTLY what SQLite's FTS5 tokenizers do to a character (the programme search's trigram
// remove_diacritics 1 and unicode61 remove_diacritics 2 fold alike): case in most cased scripts (FTS5
// leaves some capitals as they are — Cherokee, Georgian Mtavruli, some of Latin Extended-D), and the
// accents they remove (É → e, ǎ → a, ộ → o, Ｑ → ｑ; Greek keeps its tonos, final ς → σ); a
// combining accent they drop altogether folds to 0. The table is generated from SQLite itself
// (core/FtsFoldTable.h, tools/fold/gen_fts_fold.py), and RabbitEarsCli --selftest checks every BMP
// and plane-1 character against the vendored SQLite (Deseret is FTS5's one fold beyond the BMP).
// It marks the matched text in a programme-search result (common/db/Database.cpp), so a title the
// index found for "quebec" has its "Québec" marked — and no two characters are equated that the
// index's fold keeps apart.
//
// searchFold — the TV guide's channel-name matching (Win32/ui/EpgGuideControl.cpp): ftsFold, but ONE
// character for one (a character ftsFold drops is kept as it is, so a position in the folded text is
// the same position in the original), and ALSO folding Ø Đ Ħ Ŀ Ł Ŧ (a stroke, or Ŀ's middle dot) to
// their base letter, which FTS5 keeps: "lodz" finds the channel "TVP3 Łódź" there, while a programme search for
// "lodz" does not find "Łódź". (RecordingRules.cpp's foldChar is a different fold: case only, for
// Latin-1, Latin Extended-A, Romanian Ș Ț, Greek and basic Cyrillic, with accents kept (Ș → ș, not s):
// a recording rule's title match is deliberately stricter than "find what I typed".)
//
// wchar_t is 16-bit on Windows and 32-bit on macOS: on Windows a character outside the BMP is two
// UTF-16 units, each folding to itself, so Deseret's fold applies on macOS only.
//
// Header-only + inline (like FeatureFlags.h): any translation unit on either platform can use it with
// no build-file change.
#pragma once

#include <algorithm>
#include <cstdint>

#include "core/FtsFoldTable.h"

namespace rabbitears {

// `cp` as FTS5's tokenizers fold it, or 0 when they drop it. Any code point.
inline char32_t ftsFoldCodePoint(char32_t cp) {
    if (cp < 0x80) return (cp >= U'A' && cp <= U'Z') ? cp + 32 : cp;
    if (cp <= 0xFFFF) {
        const std::uint16_t* end = fts_fold::kFrom + fts_fold::kCount;
        const std::uint16_t* it = std::lower_bound(fts_fold::kFrom, end, static_cast<std::uint16_t>(cp));
        return (it != end && *it == cp) ? static_cast<char32_t>(fts_fold::kTo[it - fts_fold::kFrom]) : cp;
    }
    if (cp >= 0x10400 && cp <= 0x10427) return cp + 40;  // Deseret capitals
    return cp;
}

inline wchar_t ftsFold(wchar_t c) {
    return static_cast<wchar_t>(ftsFoldCodePoint(static_cast<char32_t>(static_cast<unsigned long>(c))));
}

inline wchar_t searchFold(wchar_t c) {
    switch (static_cast<unsigned long>(c)) {
        case 0xD8: case 0xF8: return L'o';                          // Ø ø
        case 0x110: case 0x111: return L'd';                        // Đ đ
        case 0x126: case 0x127: return L'h';                        // Ħ ħ
        case 0x13F: case 0x140: case 0x141: case 0x142: return L'l';  // Ŀ ŀ Ł ł
        case 0x166: case 0x167: return L't';                        // Ŧ ŧ
        default: break;
    }
    const wchar_t f = ftsFold(c);
    return f ? f : c;
}

}  // namespace rabbitears
