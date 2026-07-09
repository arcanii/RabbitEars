// SPDX-License-Identifier: GPL-3.0-or-later
// RecordingScheduler — the pure decision core for the recording scheduler.
//
// Given the current schedule queue + wall-clock time, it decides which schedules to
// start, stop, or mark missed. No Win32, no libVLC, no DB — so the timing/conflict
// logic (the risky part, unverifiable in a headless sandbox otherwise) is unit-tested
// in the CLI selftest. The platform layer applies the returned plan: actually driving
// VlcPlayer and writing the status changes back.
#pragma once

#include <vector>

#include "models/ScheduledRecording.h"

namespace rabbitears {

struct SchedulerPlan {
    std::vector<long long> start;  // schedule ids to begin recording now
    std::vector<long long> stop;   // schedule ids whose recording should end now
    std::vector<long long> miss;   // pending schedule ids whose window has fully passed
};

// Decide at `nowUtc` what to do. `manualRecordingActive` = a NON-scheduled recording is
// holding the single shared recorder. At most ONE schedule is started per call (one
// recorder); a schedule whose window is open while the recorder is busy simply stays
// Pending and is retried on the next call, until its stop time, when it becomes a miss.
SchedulerPlan planScheduler(const std::vector<ScheduledRecording>& schedules, long long nowUtc,
                            bool manualRecordingActive);

}  // namespace rabbitears
