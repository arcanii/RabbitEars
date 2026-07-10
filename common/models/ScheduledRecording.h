// SPDX-License-Identifier: GPL-3.0-or-later
// ScheduledRecording — one queued recording (mirrors the `scheduled_recordings`
// table). Rows are self-contained: the stream URL + playback hints are captured when
// the schedule is created, so recording still works if the source channel/playlist
// later changes or is removed. Fired by the in-app scheduler (see
// core/RecordingScheduler) while the app is running.
#pragma once

#include <string>

namespace rabbitears {

enum class ScheduleStatus {
    Pending = 0,    // waiting for its start time
    Recording = 1,  // currently recording (owns the single recorder)
    Done = 2,       // completed normally
    Missed = 3,     // start/stop window passed without recording (app closed, or recorder busy)
    Failed = 4,     // the recorder failed to start
    Cancelled = 5,  // cancelled by the user
};

struct ScheduledRecording {
    long long      id = 0;
    std::wstring   channelId;    // tvg-id it came from (reference; empty for manual entries)
    std::wstring   channelName;
    std::wstring   streamUrl;    // resolved at schedule time — recording is self-contained
    std::wstring   userAgent;
    std::wstring   referrer;
    std::wstring   title;        // programme title / user-entered label
    long long      startUtc = 0; // unix epoch seconds (UTC)
    long long      stopUtc = 0;
    std::wstring   mux = L"ts";   // "ts" | "mkv" | "mp4" (matches recFormat / VlcPlayer mux)
    ScheduleStatus status = ScheduleStatus::Pending;
    std::wstring   filePath;     // output path, set when recording starts
    long long      createdAt = 0;
    // The RecordingRule that generated this row (0 == a one-off / manually created schedule).
    // Set by core/RecordingRules::expandRules; lets a rule's still-Pending rows be dropped
    // when the rule is deleted, and keeps the expander from re-creating a row it already made.
    long long      ruleId = 0;
};

}  // namespace rabbitears
