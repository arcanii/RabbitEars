// SPDX-License-Identifier: GPL-3.0-or-later
// RecordingScheduler — the pure decision core for the recording scheduler.
//
// Given the current schedule queue + wall-clock time, it decides which schedules to
// start, stop, or mark missed. No Win32, no libVLC, no DB — so the timing/conflict
// logic (the risky part, unverifiable in a headless sandbox otherwise) is unit-tested
// in the CLI selftest. The platform layer applies the returned plan: actually driving
// VlcPlayer and writing the status changes back.
#pragma once

#include <functional>
#include <map>
#include <string>
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

// How an attempt to begin one planned start ended. See beginScheduledStart.
enum class ScheduledStart {
    // status=Recording landed AND startRecorder() reported success: commit ownership now.
    // ⚠ "Success" means only what the callback can see. On Win32 VlcPlayer::startRecording merely
    // QUEUES the start; a later libVLC failure inside the worker's doRecordStart is logged and not
    // seen here, and the row then reads Recording with nothing recording until its stop time.
    // That gap predates this function and is not closed by it (see Win32/BACKLOG.md).
    Started,
    // The Recording write was LOST. The recorder was NOT started and the row is still Pending, so
    // the next planScheduler call re-emits it — a clean retry.
    NotPersisted,
    // The Recording write landed but startRecorder() refused SYNCHRONOUSLY (on Win32: no libVLC
    // instance). The caller must write Failed, or the row reads Recording with nothing recording.
    RecorderFailed,
};

// Begin ONE schedule from SchedulerPlan::start, in the only order that cannot lose it: persist
// status=Recording FIRST, and start the recorder only if that write landed.
//
// The order is the whole point. planScheduler learns that a SCHEDULE holds the recorder only from
// the rows' status column (its one recorder-derived input, manualRecordingActive, is about
// recordings no schedule owns). Starting first and writing second meant that a write lost under DB
// contention — a VOD sync holding the writer lock past busy_timeout — left the row Pending while the
// recorder ran, so the next call re-emitted the same start. Its blocking stop finalised the file in
// progress as a truncated fragment and began another; each further lost write repeated that. The
// lost write itself was never logged. Persisting first makes a lost write harmless, because nothing
// has started yet. (Flagged by the macOS team, 2026-08-09 — Win32/BACKLOG.md.)
//
// Both callbacks belong to the platform: `persistRecording` writes the row
// (Database::updateScheduleStatus(id, Recording, path)) and reports whether it LANDED;
// `startRecorder` drives the player. Kept here, callback-shaped, so the CLI selftest executes this
// exact function against a genuinely locked database rather than a copy of its logic.
ScheduledStart beginScheduledStart(const std::function<bool()>& persistRecording,
                                   const std::function<bool()>& startRecorder);

// ---- Lost terminal-status writes (write-behind) -------------------------------------------------
//
// A terminal status (Done / Missed / Failed / Cancelled / Skipped) the platform has DECIDED but whose
// DB write was lost. planScheduler reads the status column, so an un-landed decision is not a stale
// label: a row left at Recording blocks every other schedule, and a row left Pending records the
// airing the user cancelled. So the platform keeps such decisions, replays them each tick until they
// land (flushPendingWrites), and shows them to the planner meanwhile (overlayPendingWrites).
//
// ⚠ Each decision is pinned to its row's IDENTITY, never to the id alone. scheduled_recordings.id is
// a plain INTEGER PRIMARY KEY (no AUTOINCREMENT), so SQLite hands a deleted top id to the NEXT insert
// — and a decision keyed by id would then stamp itself onto that unrelated new schedule (caught by
// adversarial review). No statement ever UPDATEs start_utc, stop_utc, created_at or stream_url: the
// only in-place edit is Database::updateScheduleStatus, which writes status and file_path. So the
// identity fields below are fixed for the life of a row. ⚠ A feature that ever edits one of them in
// place must first flush (or re-pin) any pending entry for that id — otherwise pendingWriteTarget
// reports Reused and the decision is dropped unwritten.
struct PendingStatusWrite {
    ScheduleStatus status = ScheduleStatus::Pending;
    long long      startUtc = 0;
    long long      stopUtc = 0;
    long long      createdAt = 0;
    std::wstring   streamUrl;
};
using PendingStatusWrites = std::map<long long, PendingStatusWrite>;  // keyed by schedule id

// Pin `status` to `row`'s identity.
PendingStatusWrite pendingWriteFor(const ScheduledRecording& row, ScheduleStatus status);

// Where a remembered decision stands against a fresh listing of the rows.
enum class PendingWriteTarget {
    SameRow,  // its row is listed and is the SAME row — the decision still applies
    Absent,   // no row has its id: deleted, or missed by this listing. Identity unverifiable, so
              // nothing is written or overlaid.
    Reused,   // its id now belongs to a DIFFERENT row — the decision's row is gone for good
};
PendingWriteTarget pendingWriteTarget(const std::vector<ScheduledRecording>& rows, long long id,
                                      const PendingStatusWrite& w);

// Apply every decision whose row is still the SAME row onto `rows` (the planner's view of them).
void overlayPendingWrites(std::vector<ScheduledRecording>& rows, const PendingStatusWrites& pending);

// Replay decisions against `rows`, a fresh listing. SameRow: written via `write`, removed once it
// lands. Reused: removed UNWRITTEN. Absent: kept, unwritten. An absent id is either a deleted row (an
// UPDATE would change nothing and still wait on the writer lock) or a row this listing MISSED
// (listSchedules reports a failed or partial read as fewer rows) — and a missed row cannot be
// identity-checked, so writing by id alone could stamp the decision onto whatever row holds that id
// now. The next listing that includes it resolves it: SameRow is written, Reused is dropped.
// After the FIRST write that comes back lost, no further writes are attempted in this call — there
// is one writer lock, so the rest would each wait out busy_timeout too. Returns how many landed.
int flushPendingWrites(const std::vector<ScheduledRecording>& rows, PendingStatusWrites& pending,
                       const std::function<bool(long long id, ScheduleStatus status)>& write);

}  // namespace rabbitears
