// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/RecordingScheduler.h"

namespace rabbitears {

SchedulerPlan planScheduler(const std::vector<ScheduledRecording>& schedules, long long nowUtc,
                            bool manualRecordingActive) {
    SchedulerPlan plan;

    // Is the single recorder already busy? Either a manual recording, or a schedule that
    // is already mid-record. (A schedule being stopped this same call still counts as busy,
    // so a waiting schedule doesn't try to grab a recorder that frees only after this tick.)
    bool recorderBusy = manualRecordingActive;
    for (const auto& s : schedules)
        if (s.status == ScheduleStatus::Recording) recorderBusy = true;

    for (const auto& s : schedules) {
        if (s.status == ScheduleStatus::Recording) {
            if (nowUtc >= s.stopUtc) plan.stop.push_back(s.id);  // window ended → stop
            continue;
        }
        if (s.status != ScheduleStatus::Pending) continue;  // Done/Missed/Failed/Cancelled: inert

        if (nowUtc >= s.stopUtc) {
            plan.miss.push_back(s.id);  // whole window passed without ever recording
        } else if (nowUtc >= s.startUtc) {
            // In its window. Claim the recorder iff it is free; else stay Pending and retry.
            if (!recorderBusy) {
                plan.start.push_back(s.id);
                recorderBusy = true;  // at most one start per call
            }
        }
        // else: not started yet (future) — nothing to do.
    }
    return plan;
}

}  // namespace rabbitears
