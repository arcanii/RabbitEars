// SPDX-License-Identifier: GPL-3.0-or-later
//
// A playlist's guide link can name SEVERAL XMLTV guides: an M3U header's x-tvg-url / url-tvg may list
// them separated by commas ("https://a/epg.xml.gz,https://b/epg.xml.gz"). One download of the whole
// list asks the first guide's host for a nonsense path ("/epg.xml.gz,https://b/…" — WinHTTP parses it
// as one URL); none of the owner's playlists has such a list. The link is still stored in
// playlists.epg_url (no schema change) — from the M3U as written; Set Guide URL re-joins a typed list
// with commas — and Refresh Guide splits it here, fetches each guide, and merges what they hold.
//
// Header-only + inline (like SearchFold.h): either platform can use it with no build-file change.
// Win32 uses it (MainWindowCommands: Refresh Guide, the Set Guide URL prompt; platform/UrlRedact's
// guideUrlList; platform/Log's addSecretsFromUrl); mac does not yet — its Refresh Guide still hands the
// whole link to one download.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/RecordingRules.h"  // normaliseTvgId
#include "models/Programme.h"

namespace rabbitears {

namespace guide_sources_detail {
inline bool isListSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }
inline bool isListSeparator(wchar_t c) { return c == L',' || isListSpace(c); }
// Does `s` continue at `at` with "http://" or "https://" (the scheme in any case)?
inline bool httpSchemeAt(const std::wstring& s, size_t at) {
    for (const wchar_t* scheme : {L"http://", L"https://"}) {
        size_t k = 0;
        for (; scheme[k] && at + k < s.size(); ++k) {
            wchar_t c = s[at + k];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
            if (c != scheme[k]) break;
        }
        if (!scheme[k]) return true;
    }
    return false;
}
}  // namespace guide_sources_detail

// The guide links in `link`, in order. It is split only where a run of commas and white space is
// followed by "http://" or "https://" — so a comma inside one address (in its query) stays in it — and
// each piece loses its surrounding white space. When it splits, it is a list of links: text before the
// first link is dropped (a label saved with it, "EPG Link : http://…"), and so is a run of separators
// after the last ("a,b,"). Empty pieces and repeats are dropped. A link with no such separator is ONE
// piece, as written (trimmed); a blank one is none.
inline std::vector<std::wstring> splitGuideUrls(const std::wstring& link) {
    using namespace guide_sources_detail;
    std::vector<std::pair<size_t, size_t>> pieces;  // [begin, end) in `link`
    size_t start = 0;
    for (size_t j = 1; j < link.size(); ++j) {
        if (!isListSeparator(link[j - 1]) || !httpSchemeAt(link, j)) continue;
        size_t runStart = j - 1;  // back over the whole run of separators
        while (runStart > start && isListSeparator(link[runStart - 1])) --runStart;
        pieces.emplace_back(start, runStart);
        start = j;
    }
    size_t end = link.size();
    if (!pieces.empty())
        while (end > start && isListSeparator(link[end - 1])) --end;
    pieces.emplace_back(start, end);
    const bool list = pieces.size() > 1;
    std::vector<std::wstring> out;
    for (auto [b, e] : pieces) {
        while (b < e && isListSpace(link[b])) ++b;
        while (e > b && isListSpace(link[e - 1])) --e;
        if (b == e || (list && !httpSchemeAt(link, b))) continue;
        std::wstring piece = link.substr(b, e - b);
        bool repeat = false;
        for (const auto& have : out) repeat = repeat || have == piece;
        if (!repeat) out.push_back(std::move(piece));
    }
    return out;
}

// One guide made from several. Each channel's programmes come from the FIRST guide (in the link's
// order) that lists the channel with a programme the store keeps (Programme::isValid — one whose start
// could not be read claims nothing), never a mix of two guides' schedules for one channel — their
// times and titles may not agree. Channels are compared as the TV Guide joins them (normaliseTvgId: the
// part before '@', ASCII lower case). The first guide is taken whole, as a single guide always was (the
// store drops its invalid programmes); from each later one, the programmes of the channels it owns.
// `kept[k]` / `dropped[k]` count guide k's programmes taken / left out. Consumes `guides`.
struct GuideMerge {
    std::vector<Programme> programmes;
    std::vector<size_t>    kept, dropped;
};
inline GuideMerge mergeGuideSources(std::vector<std::vector<Programme>>&& guides) {
    GuideMerge m;
    m.kept.assign(guides.size(), 0);
    m.dropped.assign(guides.size(), 0);
    if (guides.empty()) return m;
    // Which guide each channel belongs to. (A guide usually lists a channel's programmes together, so
    // each loop normalises an id only when it differs from the one before; out of order is still right.)
    std::unordered_map<std::wstring, size_t> owner;
    for (size_t k = 0; k < guides.size(); ++k) {
        const std::wstring* prev = nullptr;
        for (const Programme& p : guides[k]) {
            if (!p.isValid()) continue;
            if (!prev || p.channelId != *prev) owner.try_emplace(normaliseTvgId(p.channelId), k);
            prev = &p.channelId;
        }
    }
    // Which of the later guides' programmes are taken — first, so the result is reserved exactly.
    std::vector<std::vector<char>> take(guides.size());
    size_t total = guides[0].size();
    for (size_t k = 1; k < guides.size(); ++k) {
        take[k].assign(guides[k].size(), 0);
        std::wstring lastId;
        bool haveLast = false, lastOwned = false;
        for (size_t i = 0; i < guides[k].size(); ++i) {
            const Programme& p = guides[k][i];
            if (!p.isValid()) continue;
            if (!haveLast || p.channelId != lastId) {
                lastId = p.channelId;
                haveLast = true;
                const auto it = owner.find(normaliseTvgId(lastId));
                lastOwned = it != owner.end() && it->second == k;
            }
            take[k][i] = lastOwned ? 1 : 0;
            total += lastOwned ? 1 : 0;
        }
    }
    m.programmes = std::move(guides[0]);
    m.kept[0] = m.programmes.size();
    m.programmes.reserve(total);
    for (size_t k = 1; k < guides.size(); ++k) {
        for (size_t i = 0; i < guides[k].size(); ++i) {
            if (take[k][i]) {
                m.programmes.push_back(std::move(guides[k][i]));
                ++m.kept[k];
            } else {
                ++m.dropped[k];
            }
        }
        std::vector<Programme>().swap(guides[k]);  // free each guide once merged
    }
    return m;
}

}  // namespace rabbitears
