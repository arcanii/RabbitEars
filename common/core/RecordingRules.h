// SPDX-License-Identifier: GPL-3.0-or-later
// RecordingRules — the pure expansion core for EPG-driven ("series") recording rules.
//
// A RecordingRule says "record every airing whose title matches, on this channel". This
// turns a set of rules + the stored EPG programmes into concrete ScheduledRecording rows.
// The existing RecordingScheduler then fires those rows exactly as it fires one-off ones,
// so rules add NO new timing logic — only row generation.
//
// No Win32, no libVLC, no DB: the matching/dedup/padding rules (the risky part) are
// unit-tested headlessly via `RabbitEarsCli --selftest`. The caller owns the database and
// must fill each returned row's streamUrl / userAgent / referrer (resolved from the channel)
// and createdAt before inserting — the core cannot look a channel up.
#pragma once

#include <string>
#include <vector>

#include "models/Programme.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"

namespace rabbitears {

// How far ahead rules materialize schedules. Long enough that a weekly series is queued well
// before it airs (and survives the app being closed for days), short enough that the queue
// stays readable and a cancelled airing isn't re-offered months out.
inline constexpr long long kRuleHorizonSeconds = 14LL * 24 * 3600;  // 14 days

// Minimum gap between unforced expansions. Expansion queries a 14-day EPG window across every
// enabled playlist, which is far too heavy for the ~30 s scheduler tick on a large guide. New
// airings only appear when the guide is refreshed, so the tick is just a backstop; the guide
// refresh, a new rule, and re-enabling a rule all force an immediate pass.
inline constexpr long long kRuleExpandIntervalSeconds = 15 * 60;  // 15 minutes

// The comparable form of a tvg-id: an iptv-org '@feed' quality suffix stripped ("CNN.us@SD"
// -> "cnn.us") and ASCII-lowercased. EPG ids and playlist ids often disagree on both, so
// every channel-id comparison in the rule engine goes through this.
std::wstring normaliseTvgId(const std::wstring& tvgId);

// Expand `rules` against `programmes`, returning the schedules that should be INSERTED.
//
// A programme is scheduled when: its rule is enabled, the channel matches (or the rule has
// an empty channelId, meaning any channel), the title matches per RuleMatch, the programme
// has not already ended (stopUtc > nowUtc), and it starts at or before `horizonUtc`.
// The row's window is padded by the rule's leadSec/trailSec.
//
// Slot dedup is by (normalised channel id, padded start) against `existing` — which must contain
// ALL schedules regardless of status. That is deliberate: a slot the user Cancelled, or one
// already Done/Missed, must never be silently recreated on the next expansion pass. It also
// collapses two rules that match the same airing down to one recording.
//
// EPISODE dedup then drops a REPEAT airing of a show already scheduled: a candidate is skipped
// when some `existing` row (any status) shares its folded title AND its episode key (from
// Programme::episodeNum, else subTitle). A programme with neither carries no episode identity and
// falls back to slot dedup only. Scoping by title keeps a Contains rule spanning two series from
// cross-deduping on a shared "S01E05". Each returned row's episodeKey is set for this to persist.
std::vector<ScheduledRecording> expandRules(const std::vector<RecordingRule>& rules,
                                            const std::vector<Programme>& programmes,
                                            const std::vector<ScheduledRecording>& existing,
                                            long long nowUtc, long long horizonUtc);

}  // namespace rabbitears
