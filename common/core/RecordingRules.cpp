// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RecordingRules.h"

#include <algorithm>
#include <cwctype>
#include <map>
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

// A stable identity for "the same episode" across airings, for episode-level dedup. Built from the
// XMLTV <episode-num> AND <sub-title> together, folded (whitespace dropped, lowercased) so trivial
// format wobble (e.g. the spaces in xmltv_ns "0 . 4 . 0/1") doesn't defeat it. Both are combined
// deliberately: an <episode-num> alone can be non-identifying — a partial xmltv_ns value like
// "0 . . " (season known, episode blank) or a pretty-printed empty element folds to dots/nothing
// and would collapse EVERY episode of a series onto one key. Pairing it with the sub-title keeps
// distinct episodes distinct, while a real repeat (same num AND same sub-title) still dedups.
// Empty only when the programme carries neither field -> dedup by airing slot alone.
std::wstring episodeKey(const Programme& p) {
    auto fold = [](const std::wstring& s) {
        std::wstring o;
        o.reserve(s.size());
        for (wchar_t c : s)
            if (!std::iswspace(c)) o.push_back(static_cast<wchar_t>(std::towlower(c)));
        return o;
    };
    const std::wstring n = fold(p.episodeNum), s = fold(p.subTitle);
    if (n.empty() && s.empty()) return std::wstring();
    return L"n:" + n + L"|s:" + s;
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

    // Padding-independent airing identity. The slot key above uses the PADDED start, which is
    // a poor identity for the airing: editing a rule's leadSec changes it, so an existing row
    // created under the old padding no longer matches and the airing is re-created — a
    // mid-recording lead edit spawned a duplicate Pending row that could never start (the
    // recorder was busy with the real one) and rotted into a phantom Missed; a Cancelled
    // future airing's tombstone was silently resurrected. But every rule-created row was
    // built as [start-lead, stop+trail] with lead/trail >= 0 (both editors clamp), so its
    // window always CONTAINS its programme's unpadded window no matter how the padding has
    // since been edited — "some rule row's window contains this programme's own window" is
    // the padding-proof form of "this airing already has a row". Manual rows (ruleId == 0)
    // are deliberately excluded and keep slot-only dedup: a long manual recording spanning
    // an airing must not suppress the rule's row for it.
    std::map<std::wstring, std::vector<std::pair<long long, long long>>> ruleWindows;
    for (const ScheduledRecording& s : existing)
        if (s.ruleId != 0)
            ruleWindows[normaliseTvgId(s.channelId)].emplace_back(s.startUtc, s.stopUtc);

    // Episode dedup seed: a show already queued/recorded (any status) claims its episode, so a
    // later airing of the SAME episode is skipped. Keyed by folded title + episode key; rows with
    // no episode key (manual / pre-v6 / no-episode-num) don't participate — they slot-dedup only.
    std::set<std::pair<std::wstring, std::wstring>> takenEpisodes;
    for (const ScheduledRecording& s : existing)
        if (!s.episodeKey.empty()) takenEpisodes.emplace(foldTitle(s.title), s.episodeKey);

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
            const long long start = std::max<long long>(0, p.startUtc - r.leadSec);

            // Slot dedup: an existing row (any status) or an already-created row owns this airing.
            if (!taken.emplace(c.chan, start).second) continue;
            // Padding-independent dedup (see the ruleWindows comment above): a rule row whose
            // window contains this programme's unpadded window owns the airing, whatever
            // padding it was created under. Checked before the episode claim below so a
            // skipped candidate cannot claim an episode it did not schedule.
            if (const auto it = ruleWindows.find(c.chan); it != ruleWindows.end()) {
                bool owned = false;
                for (const auto& [ws, we] : it->second)
                    if (ws <= p.startUtc && we >= p.stopUtc) { owned = true; break; }
                if (owned) continue;
            }
            // Episode dedup: skip a repeat airing of an episode this series already has. Committed
            // only AFTER the slot check passes, so a slot-deduped candidate can't wrongly claim
            // the episode. Empty key (no episode-num/sub-title) never dedups here.
            const std::wstring ek = episodeKey(p);
            if (!ek.empty() && !takenEpisodes.emplace(c.title, ek).second) continue;

            ScheduledRecording s;
            s.ruleId = r.id;
            s.channelId = p.channelId;
            // The rule carries the display name: a channel can drop out of the library while
            // its EPG rows linger, and the schedule must still say what it is recording.
            s.channelName = r.channelName.empty() ? p.channelId : r.channelName;
            s.title = p.title;
            s.startUtc = start;
            s.stopUtc = p.stopUtc + r.trailSec;
            s.mux = r.mux;
            s.status = ScheduleStatus::Pending;
            s.episodeKey = ek;
            // The new row claims the airing padding-independently too, so a LATER rule in this
            // same pass with different lead/trail cannot create a second row for it (the
            // header's "two rules matching the same airing collapse to one recording").
            ruleWindows[c.chan].emplace_back(s.startUtc, s.stopUtc);
            out.push_back(std::move(s));
        }
    }
    return out;
}

}  // namespace rabbitears
