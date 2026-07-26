// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RecordingRules.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

namespace rabbitears {
namespace {

// Case-fold one character, WITHOUT depending on the C locale.
//
// This used to be a bare std::towlower(), with a comment claiming it handled "any language". It
// did not: the app never calls setlocale(), so the CRT stays in the "C" locale where towlower only
// folds A-Z. A Contains rule for "café" therefore missed "CAFÉ", and "тв" missed "ТВ" — silently,
// which is the worst way for a recording rule to fail.
//
// common/ has to stay platform-neutral (no Win32 CharLowerW, no ICU), so this is an explicit table
// for the ranges EPG titles actually use. It is SIMPLE case folding — 1:1, no expansions like
// ß→ss — which is exactly right for matching: both sides go through it, so they agree.
wchar_t foldChar(wchar_t c) {
    if (c < 0x80) return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 0x20) : c;
    // Latin-1 Supplement: À-Þ -> à-þ, skipping × (0xD7), which is maths, not a letter.
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return static_cast<wchar_t>(c + 0x20);
    // Latin Extended-A: even/odd upper/lower pairs, with two documented exceptions.
    if (c >= 0x0100 && c <= 0x017F) {
        if (c == 0x0130) return 0x0069;             // İ (dotted capital I) -> i
        if (c == 0x0178) return 0x00FF;             // Ÿ -> ÿ (breaks the pairing)
        if (c >= 0x0139 && c <= 0x0148) return (c % 2 == 1) ? static_cast<wchar_t>(c + 1) : c;
        if (c >= 0x0179 && c <= 0x017E) return (c % 2 == 1) ? static_cast<wchar_t>(c + 1) : c;
        return (c % 2 == 0) ? static_cast<wchar_t>(c + 1) : c;
    }
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) return static_cast<wchar_t>(c + 0x20);  // Greek
    if (c >= 0x0410 && c <= 0x042F) return static_cast<wchar_t>(c + 0x20);  // Cyrillic А-Я
    if (c >= 0x0400 && c <= 0x040F) return static_cast<wchar_t>(c + 0x50);  // Cyrillic Ѐ-Џ
    // Everything else (CJK, Hebrew, Arabic, Thai…) is caseless — return it unchanged.
    return c;
}

// Programme titles are free text in any language, so both sides of every comparison go through
// this locale-independent fold.
std::wstring foldTitle(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(foldChar(c));
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
            if (!std::iswspace(c)) o.push_back(foldChar(c));  // same locale-independent fold
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
    // created under the old padding no longer matched and the airing was re-created — a
    // mid-recording lead edit spawned a duplicate Pending row that could never start (the
    // recorder was busy with the real one) and rotted into a phantom Missed; a Cancelled
    // future airing's tombstone was silently resurrected. The real identity is the
    // programme's UNPADDED start, persisted as progStartUtc (schema v7): a rule row (any
    // status) claims (channel, progStartUtc), and no padding edit can move it. Manual rows
    // (ruleId == 0, progStartUtc 0) keep slot-only dedup.
    std::set<std::pair<std::wstring, long long>> takenAirings;
    for (const ScheduledRecording& s : existing)
        if (s.ruleId != 0 && s.progStartUtc != 0)
            takenAirings.emplace(normaliseTvgId(s.channelId), s.progStartUtc);

    // Legacy fallback (pre-v7 rows: ruleId set but progStartUtc 0 — they age out of the
    // horizon within days). Their unpadded start is unrecoverable, but every rule row was
    // built as [start-lead, stop+trail] with non-negative padding, so its window CONTAINS
    // its programme's unpadded window under any later edit. Identity heuristic: same folded
    // title AND the row's window contains the programme's own window. Title-scoping keeps a
    // legacy row from swallowing a DIFFERENT programme nested in its padding; a same-title
    // nested repeat is the residual (transitional) exposure, matching what episode dedup
    // already accepts by scoping on the folded title.
    struct LegacyWindow {
        long long start, stop;
        std::wstring title;  // folded
    };
    std::map<std::wstring, std::vector<LegacyWindow>> legacyWindows;
    for (const ScheduledRecording& s : existing)
        if (s.ruleId != 0 && s.progStartUtc == 0)
            legacyWindows[normaliseTvgId(s.channelId)].push_back(
                {s.startUtc, s.stopUtc, foldTitle(s.title)});

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
            // Padding-proof dedup (see the takenAirings comment above): the airing identity is
            // (channel, unpadded programme start) — immune to lead/trail edits. Emplacing here
            // also collapses two rules matching the same airing with DIFFERENT padding onto one
            // row. Checked before the episode claim below so a skipped candidate cannot claim
            // an episode it did not schedule.
            if (!takenAirings.emplace(c.chan, p.startUtc).second) continue;
            // Legacy fallback for pre-v7 rows (no persisted progStartUtc): same title + the
            // row's window contains the programme's own window.
            if (const auto it = legacyWindows.find(c.chan); it != legacyWindows.end()) {
                bool owned = false;
                for (const auto& w : it->second)
                    if (w.start <= p.startUtc && w.stop >= p.stopUtc && w.title == c.title) {
                        owned = true;
                        break;
                    }
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
            s.progStartUtc = p.startUtc;  // persist the padding-proof airing identity (v7)
            out.push_back(std::move(s));
        }
    }
    return out;
}

}  // namespace rabbitears
