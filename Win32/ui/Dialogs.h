// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone themed modal dialogs (no AppState dependency): the About box and a
// single-line text prompt. Split out of MainWindow.cpp to keep it focused.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "models/Channel.h"             // channel pick-list for scheduleDialog
#include "models/RecordingRule.h"       // rule rows for the recording-rules manager
#include "models/ScheduledRecording.h"  // schedule rows for the manager + add dialogs
#include "ui/MiniMeter.h"  // MeterConfig for chooseMeters

namespace rabbitears {

// Modal About box: artwork + name/version + libVLC attribution + Check-for-Updates.
void showAbout(HWND parent, HINSTANCE hInst, UINT dpi);

// Modal single-line prompt (used for the Add-Playlist URL). Returns true if the
// user pressed OK, with `value` holding the edited text.
bool promptText(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                const std::wstring& label, std::wstring& value);

// First-run Terms-of-Use gate. Modal; returns true if the user accepted, false if
// they declined (the caller should then exit the app). Blocks until answered.
bool showTerms(HWND parent, HINSTANCE hInst, UINT dpi);

// Modal themed info popup: a bold one-line `summary` headline over a scrollable,
// read-only `details` body. Used for post-import results and other notices.
void showInfoDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                    const std::wstring& summary, const std::wstring& details);

// A MODELESS themed "please wait" box for long async work (e.g. downloading + parsing the
// TV guide). showLoadingDialog returns the window (or nullptr) — pump it via the main loop,
// update its message line with updateLoadingDialog as progress arrives, and closeLoadingDialog
// when done. updateLoadingDialog/closeLoadingDialog no-op on a null HWND, so callers needn't
// null-check.
HWND showLoadingDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                       const std::wstring& message);
void updateLoadingDialog(HWND dlg, const std::wstring& message);
void closeLoadingDialog(HWND dlg);

// Action chosen in the programme popup (see programmeDialog).
// RecordSeries creates a standing rule that records every future airing whose title matches
// this programme's, on this channel (see common/core/RecordingRules).
enum class ProgrammeAction { None, Play, Schedule, RecordSeries };

// Modal programme popup shown when a TV Guide entry is clicked: the programme `title` +
// `info` (channel / time / description) with Play channel / Schedule… / Close buttons.
// Returns the chosen action (None on Close/Esc).
ProgrammeAction programmeDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                                const std::wstring& info);

// Modal categories checklist (Settings → Categories…). `allGroups` is every
// distinct group title; `checked` is in/out — the initially-checked groups on
// entry, the user's final selection on exit. A live filter box, Select All /
// Clear, and a running count help wrangle large libraries. Returns true if OK was
// pressed (on Cancel, returns false and leaves `checked` untouched).
bool chooseCategories(HWND parent, HINSTANCE hInst, UINT dpi,
                      const std::vector<std::wstring>& allGroups,
                      std::set<std::wstring>& checked);

// Modal meter-setup dialog (Settings → Meters…): four rows, each with a live preview
// meter, a Look selector, and per-role colour swatches, plus a fifth "Data flow" row for
// the buffer/fluid meter (enable + live preview only — it has no Look/palette). `cfg[4]`
// (indexed by MeterKind) and `dataFlowOn` are in/out — seeded on entry, overwritten with
// the user's choices on OK. Returns true if OK was pressed (Cancel leaves both untouched).
bool chooseMeters(HWND parent, HINSTANCE hInst, UINT dpi, MeterConfig cfg[4], bool& dataFlowOn);

// Modal "New / Edit schedule": pick a channel (type-ahead combo) + set start/stop
// (DateTimePickers) + a title. On OK returns true and fills `out.channelId/channelName/
// streamUrl/userAgent/referrer/title/startUtc/stopUtc`; the caller fills mux/status/
// createdAt and stores it. `channels` is the pick list (sorted by name inside). If
// `out` arrives pre-populated (a guide programme), those values seed the fields.
bool scheduleDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::vector<Channel>& channels,
                    ScheduledRecording& out);

// Host-provided actions for the schedule manager (the host owns the recorder + DB, so it
// stops an active recording on cancel/delete). All are invoked on the UI thread.
struct ScheduleManagerCallbacks {
    std::function<std::vector<ScheduledRecording>()> list;    // current queue (fetched on refresh)
    std::function<void(long long id)> cancel;                 // cancel (stop if it's recording)
    std::function<void(long long id)> remove;                 // delete (stop if it's recording)
    std::function<void(HWND owner)>   addNew;                 // New… (opens scheduleDialog over `owner`)
    // Not a callback: the host's wake-timer preflight (platform/PowerPolicy), already rendered to
    // text so this file stays pure UI. Non-empty ⇒ a warning banner tops the list — queueing a
    // recording the OS will never wake for is the one failure the manager must not hide.
    std::wstring wakeWarning;
};

// Modal schedule manager (Settings → Scheduled Recordings…): a list of schedules with
// New / Cancel / Delete / Close. Re-queries via cb.list() after every change.
void manageSchedules(HWND parent, HINSTANCE hInst, UINT dpi, ScheduleManagerCallbacks cb);

// Host-provided actions for the recording-rules manager. The host owns the DB (and re-expands
// the rule into schedules when it is enabled). All are invoked on the UI thread.
struct RuleManagerCallbacks {
    std::function<std::vector<RecordingRule>()> list;         // current rules
    std::function<void(long long id, bool enabled)> setEnabled;
    std::function<void(long long id)> remove;                 // deletes the rule + its pending rows
};

// Modal recording-rules manager (Settings → Recording Rules…): the standing "record every
// airing" series rules, with Enable/Disable, Delete and Close. Rules are created from the TV
// Guide (a programme's "Record series" button). Re-queries via cb.list() after every change.
void manageRules(HWND parent, HINSTANCE hInst, UINT dpi, RuleManagerCallbacks cb);

}  // namespace rabbitears
