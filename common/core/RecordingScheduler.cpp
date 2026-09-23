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

ScheduledStart beginScheduledStart(const std::function<bool()>& persistRecording,
                                   const std::function<bool()>& startRecorder) {
    // Never start a recorder whose ownership the DB does not record — see the header.
    if (!persistRecording()) return ScheduledStart::NotPersisted;
    if (!startRecorder()) return ScheduledStart::RecorderFailed;
    return ScheduledStart::Started;
}

PendingStatusWrite pendingWriteFor(const ScheduledRecording& row, ScheduleStatus status) {
    PendingStatusWrite w;
    w.status = status;
    w.startUtc = row.startUtc;
    w.stopUtc = row.stopUtc;
    w.createdAt = row.createdAt;
    w.streamUrl = row.streamUrl;
    return w;
}

PendingWriteTarget pendingWriteTarget(const std::vector<ScheduledRecording>& rows, long long id,
                                      const PendingStatusWrite& w) {
    for (const ScheduledRecording& r : rows) {
        if (r.id != id) continue;
        const bool same = r.startUtc == w.startUtc && r.stopUtc == w.stopUtc &&
                          r.createdAt == w.createdAt && r.streamUrl == w.streamUrl;
        return same ? PendingWriteTarget::SameRow : PendingWriteTarget::Reused;
    }
    return PendingWriteTarget::Absent;
}

void overlayPendingWrites(std::vector<ScheduledRecording>& rows, const PendingStatusWrites& pending) {
    if (pending.empty()) return;
    for (ScheduledRecording& r : rows) {
        const auto it = pending.find(r.id);
        if (it == pending.end()) continue;
        // The identity test is the whole safeguard: a reused id must NOT inherit the old decision.
        if (pendingWriteTarget(rows, r.id, it->second) == PendingWriteTarget::SameRow)
            r.status = it->second.status;
    }
}

int flushPendingWrites(const std::vector<ScheduledRecording>& rows, PendingStatusWrites& pending,
                       const std::function<bool(long long id, ScheduleStatus status)>& write) {
    int landed = 0;
    // SQLite has ONE writer lock: once a write comes back lost, the rest would almost surely wait
    // out busy_timeout too — each one a multi-second stall on the caller's (UI) thread. So after the
    // first loss, stop writing for this call; entries are still resolved, just not written.
    bool contended = false;
    for (auto it = pending.begin(); it != pending.end();) {
        switch (pendingWriteTarget(rows, it->first, it->second)) {
            case PendingWriteTarget::SameRow:
                if (!contended && write(it->first, it->second.status)) {
                    ++landed;
                    it = pending.erase(it);
                } else {
                    contended = true;
                    ++it;  // still contended — try again next time
                }
                break;
            case PendingWriteTarget::Reused:
                it = pending.erase(it);  // the decision's row no longer exists
                break;
            case PendingWriteTarget::Absent:
                ++it;
                break;
        }
    }
    return landed;
}

}  // namespace rabbitears
