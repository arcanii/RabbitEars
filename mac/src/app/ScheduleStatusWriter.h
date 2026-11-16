// SPDX-License-Identifier: GPL-3.0-or-later
//
// ScheduleStatusWriter — the mac scheduler's one way to write a schedule's TERMINAL status
// (Done / Missed / Failed / Cancelled / Skipped), the peer of Win32's setTerminalScheduleStatus /
// flushUnsyncedScheduleStatus / listSchedulesAsDecided / hasLiveDecision (MainWindowCommands.cpp).
//
// Why: Database::updateScheduleStatus can LOSE a write under contention (SQLITE_BUSY past
// busy_timeout — the VOD sync or the dead-link sweep holding the writer lock on its own
// connection), and planScheduler learns that a schedule holds the recorder only from the row's
// status column. A lost terminal write is therefore not a stale label: a row left at Recording
// blocks every other schedule, and a row left Pending records the airing the user cancelled. So a
// lost decision is REMEMBERED (pinned to its row's identity — common/core/RecordingScheduler.h
// PendingStatusWrite, never the id alone: SQLite reuses a deleted top rowid), replayed on every
// scheduler tick until it lands, overlaid on the rows the planner sees meanwhile, and flushed once
// more at quit (best-effort).
//
// Owned by MainWindowController (the scheduler); the Recordings window borrows a pointer so its
// cancel / rule edit / rule delete go through the same memory. Main-thread only, like both owners.
// Header-only pure C++ (no Cocoa), so mac/src/tools/selftest.cpp exercises it against a real DB.
#pragma once

#include <string>
#include <vector>

#include "core/RecordingScheduler.h"
#include "db/Database.h"
#include "platform/Log.h"

namespace rabbitears::mac {

class ScheduleStatusWriter {
public:
    explicit ScheduleStatusWriter(Database* db) : db_(db) {}

    // Write a terminal `status` now; if the write is lost, remember it (pinned to `row`, the
    // caller's copy from a listing taken BEFORE the write — a read-back after the loss cannot tell
    // "gone" from "read failed"). Returns whether it landed now.
    bool setTerminal(long long id, const ScheduledRecording* row, ScheduleStatus status) {
        if (!db_) return false;
        if (db_->updateScheduleStatus(id, status)) {
            pending_.erase(id);
            return true;
        }
        if (row)
            remember(*row, status, L"status write");
        else
            diag::error(L"schedule status write LOST (id " + std::to_wstring(id) +
                        L") — database busy, and it cannot be remembered: the caller had no listed "
                        L"row for it");
        return false;
    }

    // Queue a terminal decision WITHOUT attempting the write — for a caller that has just lost a
    // write on the same lock and must not stall the main thread for a second busy_timeout.
    void remember(const ScheduledRecording& row, ScheduleStatus status, const wchar_t* lostOp) {
        pending_[row.id] = pendingWriteFor(row, status);
        diag::error(L"schedule " + std::wstring(lostOp) + L" LOST (id " + std::to_wstring(row.id) +
                    L") — database busy; status " + std::to_wstring(static_cast<int>(status)) +
                    L" queued, retried on every scheduler tick while the row remains");
    }

    // Replay every remembered decision against a fresh listing. Returns true when a write was
    // LOST (the writer lock is held — the caller should not stall on more writes this tick).
    bool flush() {
        if (!db_ || pending_.empty()) return false;
        const size_t before = pending_.size();
        bool lost = false;
        const int landed = flushPendingWrites(db_->listSchedules(), pending_,
                                              [this, &lost](long long id, ScheduleStatus s) {
                                                  const bool ok = db_->updateScheduleStatus(id, s);
                                                  if (!ok) lost = true;
                                                  return ok;
                                              });
        const size_t dropped = before - static_cast<size_t>(landed) - pending_.size();
        if (landed > 0)
            diag::info(L"schedule status write(s) landed on retry: " + std::to_wstring(landed));
        if (dropped > 0)
            diag::info(L"schedule status decision(s) discarded, their row is gone and its id reused: " +
                       std::to_wstring(dropped));
        return lost;
    }

    // listSchedules() with every un-landed decision applied, so the planner acts on what the app
    // decided rather than on a row a lost write left behind.
    std::vector<ScheduledRecording> listAsDecided() {
        if (!db_) return {};
        std::vector<ScheduledRecording> rows = db_->listSchedules();
        overlayPendingWrites(rows, pending_);
        return rows;
    }

    // Does `r` still carry a decision that has not reached the DB (one that applies to THIS row)?
    bool hasLiveDecision(const std::vector<ScheduledRecording>& rows, const ScheduledRecording& r) const {
        const auto it = pending_.find(r.id);
        return it != pending_.end() && pendingWriteTarget(rows, r.id, it->second) == PendingWriteTarget::SameRow;
    }

    // The row is gone (deleted): no decision about it is left to land.
    void forget(long long id) { pending_.erase(id); }

    // At quit: one more flush, then report the decisions that still apply to an existing row.
    void flushAtQuit() {
        if (!db_ || pending_.empty()) return;
        flush();
        const std::vector<ScheduledRecording> rows = db_->listSchedules();
        size_t live = 0;
        for (const auto& [id, w] : pending_)
            if (pendingWriteTarget(rows, id, w) == PendingWriteTarget::SameRow) ++live;
        if (live > 0)
            diag::error(L"exiting with " + std::to_wstring(live) + L" schedule status write(s) still unlanded");
    }

    // A rule edit clears the rule's still-Pending predictions so it can re-expand against the new
    // criteria — but a raw-Pending row whose Skip/Cancel has not LANDED would be deleted with them,
    // and the re-expansion would then re-create the very airing the user skipped. So land what can
    // land, then spare every such row (it stays as the slot claim; the next flush makes it a real
    // tombstone). The ordinary case is one statement, exactly as before. (Win32 clearRulePredictions.)
    void clearRulePredictions(long long ruleId) {
        if (!db_) return;
        flush();  // always attempted: a lost clear is invisible and nothing retries it
        const std::vector<ScheduledRecording> rows = db_->listSchedules();
        bool spare = false;
        for (const ScheduledRecording& r : rows)
            if (r.ruleId == ruleId && r.status == ScheduleStatus::Pending && hasLiveDecision(rows, r)) {
                spare = true;
                break;
            }
        if (!spare) {
            if (!db_->clearPendingForRule(ruleId))
                diag::error(L"clearing rule " + std::to_wstring(ruleId) +
                            L"'s queued airings was LOST — database busy; the old ones stay queued and "
                            L"will record until the rule is edited again");
            return;
        }
        int removed = 0, left = 0;
        bool contended = false;
        for (const ScheduledRecording& r : rows) {
            if (r.ruleId != ruleId || r.status != ScheduleStatus::Pending || hasLiveDecision(rows, r)) continue;
            if (!contended && db_->deleteSchedule(r.id)) {
                ++removed;
            } else {
                contended = true;
                ++left;
            }
        }
        if (left > 0)
            diag::error(L"clearing rule " + std::to_wstring(ruleId) + L"'s queued airings was " +
                        (removed == 0 ? L"LOST" : L"partly LOST") + L" — database busy; " +
                        std::to_wstring(left) +
                        L" old one(s) stay queued and will record until the rule is edited again");
    }

    // Delete a rule — REFUSED (false, nothing changed) while any of its raw-Pending rows carries an
    // un-landed decision (deleteRule drops raw-Pending rows, losing a Skip another rule could then
    // re-queue, or a Missed the history promises to keep), or when the delete itself is lost. The
    // rule stays listed, so the user can simply retry. (Win32 onManageRules cb.remove.)
    bool deleteRule(long long ruleId) {
        if (!db_) return false;
        const bool contended = flush();
        const std::vector<ScheduledRecording> rows = db_->listSchedules();
        for (const ScheduledRecording& r : rows)
            if (r.ruleId == ruleId && r.status == ScheduleStatus::Pending && hasLiveDecision(rows, r)) {
                diag::error(L"recording rule delete DEFERRED (id " + std::to_wstring(ruleId) +
                            L") — an airing's un-landed status (Skip/Cancel/Missed) would be deleted "
                            L"with it (database busy); the rule is unchanged");
                return false;
            }
        // One transaction: false means NEITHER the rule nor its queued airings changed. After a
        // lost flush the lock is held, so BEGIN IMMEDIATE would only wait out busy_timeout again.
        if (contended || !db_->deleteRule(ruleId)) {
            diag::error(L"recording rule delete LOST (id " + std::to_wstring(ruleId) +
                        L") — database busy; the rule and its queued airings are unchanged");
            return false;
        }
        return true;
    }

    size_t pendingCount() const { return pending_.size(); }

private:
    Database* db_;
    PendingStatusWrites pending_;
};

}  // namespace rabbitears::mac
