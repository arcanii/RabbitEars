// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RecordingRules.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <utility>

namespace rabbitears {
namespace {

// Programme titles are free text (any language), so fold with towlower rather than the
// ASCII-only fold used for ids. Both sides of every comparison go through this.
std::wstring foldTitle(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(static_cast<wchar_t>(std::towlower(c)));
    return out;
}

}  // namespace

std::wstring normaliseTvgId(const std::wstring& tvgId) {
    std::wstring s = tvgId;
    if (const size_t at = s.find(L'@'); at != std::wstring::npos) s.resize(at);
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);  // ids are ASCII
    return s;
}

std::vector<ScheduledRecording> expandRules(const std::vector<RecordingRule>& rules,
                                            const std::vector<Programme>& programmes,
                                            const std::vector<ScheduledRecording>& existing,
                                            long long nowUtc, long long horizonUtc) {
    std::vector<ScheduledRecording> out;
    if (rules.empty() || programmes.empty() || horizonUtc < nowUtc) return out;

    // Every existing row claims its slot, whatever its status — see the header: a Cancelled
    // or Done airing must not come back. Inserting into this set as we go also dedups two
    // rules (or one rule matching a duplicated EPG entry) onto the same airing.
    std::set<std::pair<std::wstring, long long>> taken;
    for (const ScheduledRecording& s : existing)
        taken.emplace(normaliseTvgId(s.channelId), s.startUtc);

    // Fold each programme's channel id + title ONCE. A guide can hold tens of thousands of
    // rows; re-folding them per rule turned this into O(rules x programmes) allocations.
    // Unusable / past / beyond-horizon rows are dropped here so the rule loop never sees them.
    struct Candidate {
        const Programme* p;
        std::wstring chan;   // normalised
        std::wstring title;  // case-folded
    };
    std::vector<Candidate> candidates;
    candidates.reserve(programmes.size());
    for (const Programme& p : programmes) {
        if (!p.isValid() || p.stopUtc <= p.startUtc) continue;  // unusable EPG row
        if (p.stopUtc <= nowUtc) continue;                      // already finished
        if (p.startUtc > horizonUtc) continue;                  // beyond the horizon
        candidates.push_back({&p, normaliseTvgId(p.channelId), foldTitle(p.title)});
    }
    if (candidates.empty()) return out;

    for (const RecordingRule& r : rules) {
        if (!r.enabled) continue;
        const std::wstring wantTitle = foldTitle(r.titleMatch);
        if (wantTitle.empty()) continue;  // guard: an empty pattern would match everything
        const std::wstring wantChan = normaliseTvgId(r.channelId);  // empty == any channel

        for (const Candidate& c : candidates) {
            if (!wantChan.empty() && c.chan != wantChan) continue;
            const bool hit = (r.match == RuleMatch::Exact)
                                 ? (c.title == wantTitle)
                                 : (c.title.find(wantTitle) != std::wstring::npos);
            if (!hit) continue;

            const Programme& p = *c.p;
            ScheduledRecording s;
            s.ruleId = r.id;
            s.channelId = p.channelId;
            // The rule carries the display name: a channel can drop out of the library while
            // its EPG rows linger, and the schedule must still say what it is recording.
            s.channelName = r.channelName.empty() ? p.channelId : r.channelName;
            s.title = p.title;
            s.startUtc = std::max<long long>(0, p.startUtc - r.leadSec);
            s.stopUtc = p.stopUtc + r.trailSec;
            s.mux = r.mux;
            s.status = ScheduleStatus::Pending;

            if (!taken.emplace(c.chan, s.startUtc).second) continue;
            out.push_back(std::move(s));
        }
    }
    return out;
}

}  // namespace rabbitears
