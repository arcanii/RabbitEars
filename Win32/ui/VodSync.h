// SPDX-License-Identifier: GPL-3.0-or-later
//
// VodSync — the background half of the Xtream VOD catalogue sync.
//
// Fetches `get_vod_categories` + `get_vod_streams` off the UI thread, parses them with the pure
// `common/core/XtreamClient`, and writes the result through `bulkInsertChannels` +
// `retireMissingChannels`. Modelled on `DeadLinkSweep`, which is the pattern this project already
// trusts for "a worker that talks to the provider and writes to the DB".
//
// Design constraints, all of them load-bearing (Win32/docs/XTREAM_VOD.md §4):
//
//   • 🔴 `max_connections: 1`. On the owner's real line ONE connection is the entire budget, so a
//     sync that overlaps playback fights — and loses to, or kicks — the user's own stream. Hence
//     startVodSync() REFUSES while any pane is playing OR RECORDING (the recorder is a second
//     libVLC player with its own socket, and a scheduled recording runs with nothing playing), and
//     there is deliberately no timer: a sync only ever happens because the user asked for it.
//     The gate is not start-only either — starting playback cancels a sync in flight, because the
//     user's stream always wins. The dead-link checker learned all of this the expensive way.
//   • 🔴 Its OWN sqlite connection, joined in WM_DESTROY. Not merely to keep a long write off the
//     handle the UI thread shares with the scheduler tick — `retireMissingChannels()` stages its
//     keep-set in a per-CONNECTION temp table inside a transaction and is explicitly documented as
//     NOT reentrant, so running it on `AppState::db` while the UI touches that handle is the one
//     unsafe thing you can do with this DB.
//   • 🔴 Never retire against a keep-set we do not trust. An empty set deletes nothing by design
//     and a partial stage aborts, but a *successfully parsed yet mostly unusable* response is the
//     CALLER's judgement — so the trust guard lives here, reading the skip counts XtreamVodResult
//     reports, exactly as the dead-link sweep discards a run that reached too few servers.
//
// Three requests per playlist — a ~1 KB auth probe, then `get_vod_categories` and
// `get_vod_streams` (~13.5 MB, ~5 s for 43,599 movies on the measured provider). Not per-item:
// `get_vod_info` returns nothing usable for a movie, and 43,599 requests against a one-connection
// line would be abusive. Duration is cached from libVLC at play time instead.
#pragma once

#include <windows.h>

#include <string>

namespace rabbitears {
namespace mw {

struct AppState;

// Why a sync did or did not start. Everything except Started is a refusal the UI reports and
// forgets — no worker was created, nothing was written.
enum class VodSyncStart {
    Started,
    AlreadyRunning,
    PlaybackActive,     // 🔴 the max_connections gate. Not an error: stop playback/recording, retry.
    SweepActive,        // 🔴 same gate: the dead-link sweep already holds a provider connection
    NoXtreamPlaylist,   // no ENABLED playlist URL carries Xtream credentials
    DatabaseClosed,
    ThreadFailed,       // could not create the worker; nothing started, and the menu is NOT greyed
};

// How a finished run ended. `Ok` still needs its counts read — a run that inserted nothing and
// retired nothing is a legitimate "your library was already current".
enum class VodSyncResult {
    Ok,
    Cancelled,        // asked to stop (app closing) at a checkpoint; nothing was written
    NetworkError,
    AuthFailed,       // the panel answered and rejected us, or the line is not Active
    ParseError,
    EmptyCatalogue,   // parsed, but zero usable movies -> nothing inserted, nothing retired
    DatabaseError,
};

struct VodSyncReport {
    VodSyncResult result = VodSyncResult::Ok;
    int  playlists = 0;   // Xtream playlists actually synced
    int  inserted = 0;    // rows inserted or updated
    int  retired = 0;     // rows removed because the provider no longer lists them
    int  unusable = 0;    // items skipped by the parser (no stream id / no container extension)
    // The trust guard fired on at least one playlist: too much of the response was unusable to
    // believe the keep-set, so the deletion pass was SKIPPED. Inserts still happened — adding a
    // film we can play is safe; removing one on bad evidence is not.
    bool retireRefused = false;
    std::wstring detail;  // short technical reason (HTTP / JSON), NOT localized; for the log + tail
};

// Progress phases, posted as the WPARAM of WM_APP_VOD_PROGRESS. LPARAM carries a count that only
// kVodPhaseSaving uses (the number of movies about to be written).
constexpr WPARAM kVodPhaseContacting = 0;
constexpr WPARAM kVodPhaseFetching = 1;
constexpr WPARAM kVodPhaseSaving = 2;

// True when at least one ENABLED playlist looks like an Xtream line. Cheap (a URL parse per
// playlist, no network) — it drives whether the menu item exists at all, so a user whose provider
// has no API never sees an action that could only ever fail for them.
bool vodSyncAvailable(AppState* st);

// Kick off a sync on a worker thread. See VodSyncStart for every reason it may decline; only
// `Started` creates a thread. Progress and completion are posted to the UI thread.
VodSyncStart startVodSync(AppState* st);

// True while a sync is in flight (drives the menu item's enabled/greyed state).
bool vodSyncRunning();

// Ask a running sync to stop at the next checkpoint. Safe to call when none is running. A sync
// stopped before its writes reports Cancelled and has changed nothing; one stopped after them
// keeps what it already committed, because a half-written catalogue is still a valid catalogue.
void cancelVodSync();

// Join any running sync. Called from WM_DESTROY — the worker owns a DB handle and a socket, so it
// must be finished before the app tears down.
void shutdownVodSync();

// The finished run's report. Read it from the WM_APP_VOD_DONE handler; only one sync runs at a
// time and the worker fills this in before it posts, so the value is stable there.
VodSyncReport vodSyncReport();

}  // namespace mw
}  // namespace rabbitears
