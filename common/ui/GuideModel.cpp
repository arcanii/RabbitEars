// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/GuideModel.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "db/Database.h"

namespace rabbitears {

namespace {

// The same normalisation as Database's RE_NORM_TVG: the part before '@', ASCII-lower-cased.
std::wstring normId(const std::wstring& s) {
    std::wstring b = s.substr(0, s.find(L'@'));
    for (auto& ch : b)
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    return b;
}

// Catch-up: fold channel `c` (the next in channelsByPlaylist's order) into its id's pick — the longest
// archive wins, the earlier channel on a tie (Database::refreshProgrammeSearchChannels: the same rule).
void foldArchive(GuideArchivePick& pick, const Database::GuideChannel& c) {
    if (c.archiveDays > pick.days) {
        pick.channel = c.id;
        pick.days = c.archiveDays;
    }
}

}  // namespace

size_t GuideArchivePicks::KeyHash::operator()(const Key& k) const noexcept {
    return std::hash<std::wstring>{}(k.base) ^ (std::hash<long long>{}(k.playlistId) * 0x9E3779B97F4A7C15ull);
}

GuideArchivePicks::GuideArchivePicks(Database& db) {
    for (const auto& c : db.liveGuideChannels())
        if (c.archiveDays > 0) foldArchive(picks_[Key{c.playlistId, normId(c.tvgId)}], c);
}

GuideArchivePick GuideArchivePicks::pick(long long playlistId, const std::wstring& tvgId) const {
    const auto it = picks_.find(Key{playlistId, normId(tvgId)});
    return it == picks_.end() ? GuideArchivePick{} : it->second;
}

GuideModel buildGuideModel(Database& db, long long nowUtc) {
    GuideModel m;
    const long long winStart = nowUtc - kGuideWindowPastSec;
    const long long winEnd = nowUtc + kGuideWindowAheadSec;
    // The coverage line (GuideCoverage), counted alongside the rows: per playlist, its live channels'
    // distinct guide ids (one query for all playlists, ~3 ms) split into those with a row, those
    // without one in a playlist with no guide link, and those without one despite a link; and, across
    // ALL playlists, the guide's channels that matched none of the user's.
    GuideCoverage& coverage = m.coverage;
    coverage.valid = true;
    std::unordered_map<long long, int> idsByPlaylist;
    for (const auto& [pid, n] : db.distinctLiveGuideIds()) idsByPlaylist[pid] = n;
    // The channels a row can be built for, per playlist, in channelsByPlaylist's order.
    std::unordered_map<long long, std::vector<Database::GuideChannel>> chansByPlaylist;
    for (auto& c : db.liveGuideChannels()) chansByPlaylist[c.playlistId].push_back(std::move(c));
    std::unordered_set<std::wstring> guideMatched, guideUnmatched;  // normalised guide ids
    std::vector<GuideRow>& rows = m.rows;
    for (const auto& pl : db.listPlaylists()) {
        if (!pl.enabled) continue;
        const auto idsIt = idsByPlaylist.find(pl.id);
        const int ids = idsIt == idsByPlaylist.end() ? 0 : idsIt->second;
        std::unordered_set<std::wstring> rowBases;  // this playlist's ids that got a row
        auto progs = db.programmesInWindow(pl.id, winStart, winEnd);  // ordered channel_id, start
        if (!progs.empty()) {
            // Keep the FIRST channel per base — its FULL tvg-id becomes the row's channelId, which
            // Play/Schedule resolve via channelByTvgId, so every row stays playable — and the one per
            // base keeping the longest archive (catch-up, foldArchive), which may be another of them.
            struct Pick {
                std::wstring     name, tvgId;
                GuideArchivePick archive;
            };
            std::unordered_map<std::wstring, Pick> byBase;  // base -> the row's channel + archive
            const auto chIt = chansByPlaylist.find(pl.id);
            if (chIt != chansByPlaylist.end())
                for (const auto& c : chIt->second)
                    foldArchive(byBase.try_emplace(normId(c.tvgId), Pick{c.name, c.tvgId}).first->second.archive, c);
            GuideRow cur;
            std::wstring curId;
            bool have = false;     // building a row for a channel that IS in this playlist?
            bool started = false;  // entered any channel group yet? (have can no longer double as this)
            auto flush = [&] {
                if (have && !cur.programmes.empty()) rows.push_back(std::move(cur));
                cur = GuideRow{};
                have = false;
            };
            for (auto& p : progs) {
                if (!started || p.channelId != curId) {
                    flush();
                    curId = p.channelId;
                    started = true;
                    const std::wstring base = normId(curId);  // programme.channelId is the EPG base id
                    auto it = byBase.find(base);
                    if (it != byBase.end()) {
                        cur.channelId = it->second.tvgId;  // the channel's FULL tvg-id (Play/Schedule use it)
                        cur.channelName = it->second.name.empty() ? curId : it->second.name;
                        cur.archiveChannel = it->second.archive.channel;
                        cur.archiveDays = it->second.archive.days;
                        cur.playlistId = pl.id;
                        have = true;
                        guideMatched.insert(base);
                        rowBases.insert(base);
                    } else {
                        guideUnmatched.insert(base);
                    }
                }
                if (have)
                    cur.programmes.push_back({std::move(p.title), std::move(p.descr), p.startUtc, p.stopUtc});
            }
            flush();
        }
        const int shownHere = static_cast<int>(rowBases.size());  // <= ids: each is one of its live ids
        const int missing = std::max(0, ids - shownHere);
        coverage.withId += ids;
        coverage.shown += std::min(shownHere, ids);
        (pl.epgUrl.empty() ? coverage.inNoLinkPlaylists : coverage.noProgrammes) += missing;
    }
    // "Matching none of yours" means none in ANY enabled playlist — also one with no guide link of its
    // own, whose channel this guide would serve once linked (so that is not "another provider").
    std::unordered_set<std::wstring> anyLiveId;
    for (auto& id : db.liveGuideIds()) anyLiveId.insert(std::move(id));
    for (const auto& id : guideUnmatched)
        if (!guideMatched.count(id) && !anyLiveId.count(id)) ++coverage.guideUnmatched;
    m.coverageLog =
        std::to_wstring(coverage.shown) + L" of " + std::to_wstring(coverage.withId) +
        L" guide ids among the live channels have rows; " + std::to_wstring(coverage.inNoLinkPlaylists) +
        L" without one in playlists with no guide link, " + std::to_wstring(coverage.noProgrammes) +
        L" despite one; " + std::to_wstring(coverage.guideUnmatched) + L" guide channels matching none";
    std::sort(rows.begin(), rows.end(),
              [](const GuideRow& a, const GuideRow& b) { return a.channelName < b.channelName; });
    return m;
}

}  // namespace rabbitears
