// SPDX-License-Identifier: GPL-3.0-or-later
// RecordingRule — a standing "record every airing that matches this" rule (mirrors the
// `recording_rules` table). Rules are EPG-driven: core/RecordingRules expands them against
// the stored programmes into concrete ScheduledRecording rows, which the existing scheduler
// then fires. A rule is a *recipe*; the schedules it produces are the queue.
#pragma once

#include <string>

namespace rabbitears {

enum class RuleMatch {
    Exact = 0,     // the programme title equals titleMatch (case-folded)
    Contains = 1,  // the programme title contains titleMatch (case-folded)
};

struct RecordingRule {
    long long    id = 0;
    // tvg-id of the channel to watch. Empty == ANY channel (a title-only rule). Compared on
    // the normalised base id (an iptv-org '@feed' suffix stripped, case-folded).
    std::wstring channelId;
    std::wstring channelName;  // display only (the channel may later vanish from the library)
    std::wstring titleMatch;   // never empty — an empty pattern would match every programme
    RuleMatch    match = RuleMatch::Exact;
    bool         enabled = true;
    int          leadSec = 0;   // start this many seconds BEFORE the programme (clock slop)
    int          trailSec = 0;  // keep recording this many seconds after it ends (overruns)
    std::wstring mux = L"ts";   // container for the recordings this rule creates
    long long    createdAt = 0;
};

}  // namespace rabbitears
