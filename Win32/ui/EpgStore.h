// SPDX-License-Identifier: GPL-3.0-or-later
//
// EpgStore — a guide refresh's database write, off the UI thread.
//
// Refresh Guide's fetch worker downloads and parses; this stores what it parsed. The store used to run
// on the UI thread (onEpgDone): on the owner's ~200k programmes, 0.8–1.3 s to store and ~1.5 s to
// re-index them for the guide's search, with the window frozen behind the loading box. Now a worker
// does both with its OWN connection, and the UI thread keeps only what needs AppState: the
// recording-rule pass and the result (onEpgStored).
//
// Its own connection rather than AppState::db: that handle is FULLMUTEX, so a long write on it from
// here would stall every UI-thread query behind it. On a second connection, WAL lets the UI keep
// reading; a UI-thread WRITE made meanwhile waits for the transaction in progress, and is LOST if it
// has to wait past busy_timeout (5 s) — the scheduler's status writes are retried (PendingStatusWrite),
// most others are not. Hence one short transaction per playlist, then one for the search index, with
// a pause between them, rather than one long one.
//
// Joined in WM_DESTROY after the players, like the other workers; cancelEpgStore() stops it between
// transactions, so the join waits for the one in progress (or the 250 ms pause before the next) —
// plus, if another connection holds the write lock just then, that transaction's BEGIN waiting
// (busy_timeout, up to 5 s). A transaction cut short by the exit watchdog rolls back, leaving that
// playlist's previous guide.
#pragma once

#include <memory>

#include <windows.h>

namespace rabbitears {
namespace mw {

struct EpgResult;

// Store `res`'s successfully fetched guides on a worker: progress lines go to the loading box
// (WM_APP_EPG_PROGRESS), then WM_APP_EPG_STORED is posted and takeEpgStoreResult() hands `res` back
// with the store's results filled in. Always ends that way — if the worker cannot start, the store
// runs on the calling thread before this returns. One at a time (Refresh Guide is busy-guarded).
void startEpgStore(HWND hwnd, std::unique_ptr<EpgResult> res);

// The finished store, once WM_APP_EPG_STORED arrives; null if there is none.
std::unique_ptr<EpgResult> takeEpgStoreResult();

// True from startEpgStore until the worker has published its result. The TV Guide's search does not
// rebuild a stale index meanwhile: the worker is about to, and would only be kept waiting for the lock.
bool epgStoreRunning();

// Stop at the next transaction boundary (WM_DESTROY, early), then wait for it (shutdownEpgStore).
void cancelEpgStore();
void shutdownEpgStore();

}  // namespace mw
}  // namespace rabbitears
