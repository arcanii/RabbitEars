// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window — command handlers + multi-player panes (split from MainWindow.cpp).
#include "ui/MainWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>  // SHGetKnownFolderPath (Videos folder for recordings)
#include <objidl.h>  // IStream — required by gdiplus.h below
#include <windowsx.h>
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it. (Used for the
// process-wide GDI+ startup the About box's rendering relies on.)
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Log.h"
#include "platform/Updater.h"
#include "resource.h"
#include "version.h"
#include "ui/BufferMeter.h"
#include "ui/ChannelGridControl.h"
#include "ui/Dialogs.h"
#include "ui/DockLayout.h"
#include "ui/EpgGuideControl.h"
#include "ui/MiniMeter.h"
#include "ui/Splash.h"
#include "ui/Theme.h"
#include "ui/VideoGrid.h"
#include "ui/VlcEngine.h"
#include "ui/VlcPlayer.h"

#include "audio/SpectrumTap.h"

#include "ui/MainWindowInternal.h"  // AppState + shared types/ids (rabbitears::mw)

#ifdef RABBITEARS_THEME_ENGINE
#include "platform/Encoding.h"   // wideFromUtf8 / utf8FromWide for the skin settings key + value
#include "ui/skin/SkinStrip.h"  // Phase-1 GPU skin spike: the transport-strip underglow surface
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

namespace rabbitears {
namespace mw {

// ---- command handlers ------------------------------------------------------

void onAddUrl(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A playlist is still loading — please wait…");
        diag::warn(L"Add Playlist ignored: a playlist load is already in progress");
        return;
    }
    std::wstring url;  // no bundled/default playlist — users supply their own source
    if (!promptText(st->hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                    st->dpi, L"Add Playlist", L"Playlist URL (.m3u / .m3u8):", url))
        return;
    if (url.empty()) return;
    startPlaylistWorker(st, url, true, nameFromSource(url, true));
}

void onOpenFile(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A playlist is still loading — please wait…");
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"Playlists (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) startPlaylistWorker(st, path, false, nameFromSource(path, false));
}

void onPlaylistDone(AppState* st, PlaylistResult* res) {
    st->busy = false;
    std::wstring summary, details;
    const std::wstring src = L"Source:  " + res->source + L"\r\n\r\n";
    if (res->ok && !res->doc.channels.empty()) {
        const long long now = static_cast<long long>(time(nullptr));
        const long long pid = st->db.addPlaylist(res->name, res->source, res->isUrl, now, res->doc.epgUrl);
        if (pid == 0) {
            setStatus(st, L"Add playlist failed: could not save to the database");
            diag::error(L"playlist add failed: addPlaylist returned 0 for " + res->source);
            summary = L"Could not import the playlist";
            details = src + L"Problem:  the playlist could not be saved to the database.\r\n";
        } else {
            const int n = st->db.bulkInsertChannels(pid, res->doc.channels, now);
            res->imported = n;
            refreshNav(st);
            st->filter = {ViewKind::Playlist, L"", pid};
            loadForFilter(st);
            setStatus(st, L"Added " + std::to_wstring(n) + L" channels from " + res->name);
            diag::info(L"playlist added: \"" + res->name + L"\" (" + std::to_wstring(n) +
                       L" channels) from " + res->source);
            const int skipped = res->parsed - res->imported;
            summary = L"Imported " + std::to_wstring(res->imported) + L" channels from " + res->name;
            details = src + L"Channels parsed:  " + std::to_wstring(res->parsed) + L"\r\n" +
                      L"Channels imported:  " + std::to_wstring(res->imported) + L"\r\n";
            if (skipped > 0)
                details += L"Skipped (blank or duplicate URLs):  " + std::to_wstring(skipped) + L"\r\n";
            details += L"Groups:  " + std::to_wstring(res->groups) + L"\r\n";
        }
    } else {
        std::wstring msg = res->error;
        if (msg.empty())
            msg = res->ok ? L"The playlist contained no channels." : L"No channels found.";
        setStatus(st, L"Add playlist failed: " + msg);
        diag::error(L"playlist add failed from " + res->source + L": " + msg);
        summary = L"Could not import the playlist";
        details = src + L"Problem:  " + msg + L"\r\n";
    }
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, L"Import results", summary, details);
    delete res;
}

// Fetch + store the XMLTV guide for every enabled playlist that carries an EPG URL.
// Mirrors startPlaylistWorker: the download + gunzip + parse run on a detached worker
// (busy-guarded), then WM_APP_EPG_DONE stores the parsed programmes on the UI thread.
void onEpgRefresh(AppState* st) {
    if (st->busy) {
        setStatus(st, L"Busy — please wait…");
        return;
    }
    std::vector<EpgTarget> targets;
    for (const auto& pl : st->db.listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets.push_back({pl.id, pl.name, pl.epgUrl});

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (targets.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Refresh Guide", L"No guide source found",
                       L"None of your enabled playlists carry an XMLTV guide URL (x-tvg-url in the "
                       L"#EXTM3U header).\r\n\r\nAdd a playlist that includes one, then try again.");
        return;
    }
    st->busy = true;
    setStatus(st, L"Downloading guide…");
    st->loadingDlg =
        showLoadingDialog(st->hwnd, hInst, st->dpi, L"TV Guide", L"Contacting guide source…");
    diag::info(L"EPG refresh start: " + std::to_wstring(targets.size()) + L" playlist(s)");
    HWND hwnd = st->hwnd;
    std::thread([hwnd, targets]() {
        // Progress lines carry a heap wstring* the UI thread shows in the loading box, then frees.
        auto post = [hwnd](const std::wstring& s) {
            PostMessageW(hwnd, WM_APP_EPG_PROGRESS, 0, reinterpret_cast<LPARAM>(new std::wstring(s)));
        };
        const size_t n = targets.size();
        auto* res = new EpgResult();
        for (size_t i = 0; i < n; ++i) {
            const EpgTarget& t = targets[i];
            EpgFetch f;
            f.playlistId = t.id;
            f.name = t.name;
            const std::wstring tag =
                n > 1 ? L" (" + std::to_wstring(i + 1) + L" of " + std::to_wstring(n) + L")" : L"";
            std::string bytes;
            std::wstring err;
            post(L"Downloading " + t.name + L"…" + tag);
            if (!httpGet(t.url, bytes, err, 60000)) {  // guides are large; allow 60 s
                f.error = err.empty() ? L"download failed" : err;
            } else {
                post(L"Parsing " + t.name + L" (" + std::to_wstring(bytes.size() / 1024) + L" KB)…" +
                     tag);
                const std::string xml = gunzipIfNeeded(bytes);
                if (xml.empty())
                    f.error = L"empty or invalid after decompression";
                else
                    f.programmes = parseXmltv(xml).programmes;
            }
            res->fetches.push_back(std::move(f));
        }
        PostMessageW(hwnd, WM_APP_EPG_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

void onEpgDone(AppState* st, EpgResult* res) {
    st->busy = false;
    closeLoadingDialog(st->loadingDlg);  // dismiss the "please wait" box before the results dialog
    st->loadingDlg = nullptr;
    const long long now = static_cast<long long>(time(nullptr));
    int okCount = 0, totalProg = 0;
    std::set<std::wstring> chans;
    std::wstring detail;
    for (auto& f : res->fetches) {
        if (!f.error.empty()) {
            detail += f.name + L":  " + f.error + L"\r\n";
            diag::error(L"EPG refresh failed for \"" + f.name + L"\": " + f.error);
            continue;
        }
        const int stored = st->db.bulkInsertProgrammes(f.playlistId, f.programmes, now);
        ++okCount;
        totalProg += stored;
        for (const auto& p : f.programmes) chans.insert(p.channelId);
        detail += f.name + L":  " + std::to_wstring(stored) + L" programmes\r\n";
        diag::info(L"EPG stored " + std::to_wstring(stored) + L" programmes for \"" + f.name + L"\"");
    }
    const std::wstring summary =
        okCount > 0 ? L"Stored " + std::to_wstring(totalProg) + L" programmes across " +
                          std::to_wstring(chans.size()) + L" channels"
                    : L"Could not refresh the guide";
    setStatus(st, summary);
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, L"Refresh Guide", summary, detail);
    delete res;
}

// Assemble the timeline guide from stored programmes (all enabled playlists) and open
// the guide window. Programmes are joined to channels by tvg-id, and ONLY channels that
// exist in a playlist are shown (so every entry is playable). The whole loaded window
// scrolls client-side (no re-query).
// Defined below (after the scheduler helpers) — declared here for onEpgGuide's callback.
void scheduleFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc);

void onEpgGuide(AppState* st) {
    const long long now = static_cast<long long>(time(nullptr));
    const long long winStart = now - 6 * 3600;    // a little history
    const long long winEnd = now + 72 * 3600;     // three days ahead
    std::vector<GuideRow> rows;
    for (const auto& pl : st->db.listPlaylists()) {
        if (!pl.enabled) continue;
        auto progs = st->db.programmesInWindow(pl.id, winStart, winEnd);  // ordered channel_id, start
        if (progs.empty()) continue;
        // Index channels by their EPG id: iptv-org tvg-ids carry an "@feed" quality suffix
        // (e.g. "CNN.us@SD") while XMLTV feeds key on the base id ("CNN.us"), so match on the base,
        // case-insensitively (`normId`). Keep the FIRST channel per base — its FULL tvg-id becomes the
        // row's channelId, which Play/Schedule resolve via channelByTvgId, so every row stays playable.
        auto normId = [](const std::wstring& s) {
            std::wstring b = s.substr(0, s.find(L'@'));
            for (auto& ch : b)
                if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            return b;
        };
        std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> byBase;  // base -> (name, full tvg-id)
        for (const auto& c : st->db.channelsByPlaylist(pl.id))
            if (!c.tvgId.empty()) byBase.emplace(normId(c.tvgId), std::make_pair(c.name, c.tvgId));
        GuideRow cur;
        std::wstring curId;
        bool have = false;     // building a row for a channel that IS in this playlist?
        bool started = false;  // entered any channel group yet? (have can no longer double as this)
        auto flush = [&] {
            if (have && !cur.programmes.empty()) rows.push_back(std::move(cur));
            cur = GuideRow{};
            have = false;
        };
        for (auto& p : progs) {
            if (!started || p.channelId != curId) {
                flush();
                curId = p.channelId;
                started = true;
                auto it = byBase.find(normId(curId));  // programme.channelId is the EPG base id
                if (it != byBase.end()) {
                    cur.channelId = it->second.second;  // the channel's FULL tvg-id (Play/Schedule use it)
                    cur.channelName = it->second.first.empty() ? curId : it->second.first;
                    have = true;
                }
            }
            if (have) cur.programmes.push_back({p.title, p.descr, p.startUtc, p.stopUtc});
        }
        flush();
    }
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (rows.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"TV Guide", L"No guide to show",
                       L"No stored programmes match a channel in your playlists.\r\n\r\n"
                       L"Either run Settings ▸ Refresh Guide… first, or the guide's channel IDs don't "
                       L"match your playlist — point it at a guide whose tvg-ids line up (right-click a "
                       L"playlist ▸ Set Guide URL…).");
        return;
    }
    std::sort(rows.begin(), rows.end(),
              [](const GuideRow& a, const GuideRow& b) { return a.channelName < b.channelName; });
    GuideCallbacks cb;
    cb.onSchedule = [st](const std::wstring& channelId, const std::wstring& channelName,
                         const std::wstring& title, long long startUtc, long long stopUtc) {
        scheduleFromGuide(st, channelId, channelName, title, startUtc, stopUtc);
    };
    cb.onPlay = [st](const std::wstring& channelId, const std::wstring& channelName) {
        std::optional<Channel> ch;
        if (!channelId.empty()) ch = st->db.channelByTvgId(channelId);
        if (ch) {
            playChannel(st, *ch);
            // Starting a show hides the guide (a big window over the viewer) and brings the main
            // window forward, so the picked channel is actually visible instead of playing behind
            // the guide. Reopen 📺 TV Guide to bring it back.
            hideEpgGuide();
            SetForegroundWindow(st->hwnd);
        } else {
            // setStatus lands in the status bar, which is behind the guide — the user never saw
            // it. Say why loudly instead (almost always a tvg-id mismatch, per the guide caveat).
            HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
            showInfoDialog(st->hwnd, hi, st->dpi, L"TV Guide", L"No matching channel",
                           L"Couldn't find a channel for \"" + channelName +
                               L"\" in your playlist.\r\n\r\nThe guide matches programmes to channels "
                               L"by tvg-id — this programme's channel ID has no match. Point the "
                               L"playlist at a guide whose IDs match it (right-click the playlist "
                               L"▸ Set Guide URL…).");
        }
    };
    showEpgGuide(st->hwnd, hInst, st->dpi, std::move(rows), now, cb);
}

// Prompt for a playlist's XMLTV guide URL (seeded with its current one), save the override,
// and offer to fetch it now. Shared by the playlist context menu and the TV Guide node menu.
void promptSetGuideUrl(HWND hwnd, AppState* st, long long pid) {
    std::wstring url;  // seed with the current URL (M3U x-tvg-url or a prior override)
    for (const auto& pl : st->db.listPlaylists())
        if (pl.id == pid) { url = pl.epgUrl; break; }
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    if (!promptText(hwnd, hInst, st->dpi, L"Set Guide URL",
                    L"XMLTV guide URL (.xml or .xml.gz; blank to clear):", url))
        return;
    st->db.setPlaylistEpgUrl(pid, url);
    diag::info(L"set epg_url for playlist id=" + std::to_wstring(pid) +
               (url.empty() ? L" (cleared)" : L" to \"" + url + L"\""));
    if (url.empty()) {
        setStatus(st, L"Guide URL cleared");
    } else if (MessageBoxW(hwnd, L"Guide URL saved.\n\nDownload the guide now?", L"Set Guide URL",
                           MB_YESNO | MB_ICONQUESTION) == IDYES) {
        onEpgRefresh(st);  // fetches every enabled playlist that has a URL
    } else {
        setStatus(st, L"Guide URL saved — run Settings ▸ Refresh Guide to fetch it");
    }
}

void toggleFullscreen(AppState* st) {
    HWND hwnd = st->hwnd;
    st->fullscreen = !st->fullscreen;
    if (st->fullscreen) {
        // Real fullscreen: remember the window, drop the frame to a borderless
        // popup, and cover the whole monitor (over the taskbar). Windows treats a
        // borderless window covering the monitor as fullscreen and hides the shell.
        st->prevPlacement.length = sizeof(st->prevPlacement);
        GetWindowPlacement(hwnd, &st->prevPlacement);
        st->prevStyle = GetWindowLongW(hwnd, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (st->prevStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetFocus(st->ap().hwnd);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, st->prevStyle);
        SetWindowPlacement(hwnd, &st->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout(hwnd, st);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// "Video only": collapse the window to just the video — hide the nav list, channel grid,
// title/command bar, and transport strip — WITHOUT leaving the window (unlike fullscreen,
// which covers the whole monitor). Entered from Settings → Video only; a double-click on
// the video or Esc restores the chrome. Reuses the fullscreen layout/paint path (which
// already hides every child + fills the client with the video), minus the window-style
// change. A no-op while actually fullscreen — that mode already shows only the video.
void toggleVideoOnly(AppState* st) {
    // Entering video-only from fullscreen: drop fullscreen first (the two are mutually exclusive),
    // so "Video only" from the fullscreen right-click menu lands in windowed video-only instead of
    // doing nothing. toggleFullscreen clears st->fullscreen, so the toggle below then proceeds.
    if (st->fullscreen) toggleFullscreen(st);
    st->videoOnly = !st->videoOnly;
    layout(st->hwnd, st);
    InvalidateRect(st->hwnd, nullptr, TRUE);
    if (st->videoOnly) SetFocus(st->ap().hwnd);  // so Esc reaches VideoProc while the chrome is hidden
}

// ---- multi-player panes (split view / PIP) ---------------------------------

// Create video pane #index: a kVideoClass child window (its index stashed in GWLP_USERDATA
// so VideoProc knows which pane it is) bound to a fresh VlcPlayer that borrows the shared
// engine. Each pane posts events tagged with its index; only the active pane drives the
// transport strip. Created hidden — layout() shows the panes the current mode uses.
VideoPane* addPane(HWND hwnd, AppState* st, int index, bool floating) {  // default in the header proto
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    auto pane = std::make_unique<VideoPane>();
    pane->floating = floating;
    if (floating) {
        // PIP: a top-level TOPMOST popup owned by the main window, so DWM composites it above the
        // main window's libVLC D3D surface (a child sibling — even an owned, non-topmost popup —
        // gets drawn under that surface and stays invisible). NOACTIVATE so clicking doesn't steal
        // focus; TOOLWINDOW keeps it off the taskbar. Created hidden; positionFloatingPip() sizes +
        // shows it in SCREEN coords.
        pane->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kVideoClass,
                                     L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, hwnd, nullptr,
                                     hInst, nullptr);
    } else {
        // A tile in the video area. Pane 0 is visible from creation (parity with the old single
        // video window); extra tiles start hidden and layout() shows them.
        const DWORD style = WS_CHILD | WS_CLIPSIBLINGS | (index == 0 ? WS_VISIBLE : 0u);
        pane->hwnd = CreateWindowExW(0, kVideoClass, L"", style, 0, 0, 10, 10, hwnd, nullptr, hInst,
                                     nullptr);
    }
    SetWindowLongPtrW(pane->hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(index));
    VideoPane* raw = pane.get();
    st->panes.push_back(std::move(pane));
    raw->player.init(st->engine);
    raw->player.attach(raw->hwnd);  // the pane HWND — the pool's fallback, never a set_hwnd target
    raw->player.setEventTarget(hwnd, WM_APP_VLC);
    raw->player.setTag(index);
    raw->player.setVoutHostMsg(WM_APP_MAKE_VOUT_HOST);  // lets the worker grow the host pool on demand
    // Pre-create a small pool of vout hosts for this pane (hidden). libVLC renders into a host, never
    // the pane HWND directly: a new stream attaches to a proven-free host so the previous stream's
    // vout can drain without spawning the "VLC (Direct3D11 output)" popout on rapid channel-surf. Two
    // covers steady-state ping-pong; the worker grows the pool (WM_APP_MAKE_VOUT_HOST) under churn.
    for (int i = 0; i < 2; ++i)
        if (HWND h = makeVoutHost(st, index)) raw->player.registerVoutHost(h);
    if (index != st->active) raw->player.setMuted(true);  // only the active pane is audible
    return raw;
}

// Make pane `idx` the active one: route audio (others muted), the grid now-playing
// highlight, the play/pause glyph, the meters, and the active-pane border to it. Safe if
// out of range (no-op).
void setActivePane(AppState* st, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->panes.size())) return;
    diag::info(L"active pane -> " + std::to_wstring(idx) + L" (of " +
               std::to_wstring(st->panes.size()) + L")");
    st->active = idx;
    const int vol = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
    for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
        st->panes[i]->player.setMuted(i != idx);         // only the active pane keeps its audio track
        if (i == idx) st->panes[i]->player.setVolume(vol);
    }
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    SetWindowTextW(st->btnPlay, st->ap().player.isPlaying() ? kGlyphPause : kGlyphPlay);
    resetStatMeters(st);  // the previous pane's readings don't apply to the newly-active stream
    setStatus(st, st->ap().nowPlayingName.empty()
                      ? L"Active pane " + std::to_wstring(idx + 1)
                      : L"Active: " + st->ap().nowPlayingName);
    // erase=TRUE: the active-pane accent border is drawn in the inter-pane gap, so the PREVIOUS
    // active pane's border must be cleared (windowBg fill via WM_ERASEBKGND) — otherwise borders
    // accumulate and every tile ends up looking "active". WS_CLIPCHILDREN keeps the fill off the
    // video surfaces, so only the gaps repaint (no video flicker).
    InvalidateRect(st->hwnd, nullptr, TRUE);  // repaint the active-pane border (clearing the old one)
}

// Switch the view mode. Every pane except pane 0 is torn down and recreated for the target mode,
// because their window TYPE differs — Split tiles are child windows, the PIP pane is a floating
// top-level popup. Pane 0 (the primary child surface) always persists and keeps playing.
// Reap panes torn down by a mode switch once their async stop has finished (or force at exit).
// Runtime-safe: a pane whose reaper is still stopping a stuck stream is left for the next pass, so
// the UI thread never blocks. force=true (WM_DESTROY) drains everything synchronously — the exit
// watchdog covers a truly wedged feed.
void reapDyingPanes(AppState* st, bool force) {
    for (auto it = st->dyingPanes.begin(); it != st->dyingPanes.end();) {
        VideoPane* p = it->get();
        if (force || p->player.teardownComplete()) {
            p->player.shutdown();                 // joins the now-finished reaper instantly
            if (p->hwnd) DestroyWindow(p->hwnd);  // UI thread — safe now (the pane's vout is gone)
            it = st->dyingPanes.erase(it);
        } else {
            ++it;
        }
    }
}

void applyViewMode(AppState* st, ViewMode mode) {
    reapDyingPanes(st, /*force=*/false);  // clear any finished leftovers from a previous switch
    // Carry the SELECTED pane's stream across the collapse. Pane 0 (the primary child surface)
    // always persists, but only pane 0's HWND survives — not necessarily its channel. If a
    // NON-pane-0 tile is active (e.g. you picked the bottom-right of a 2x2), remember what it was
    // playing and replay it into pane 0 below, so leaving Split/PIP keeps what you were watching
    // instead of snapping back to the top-left tile.
    const bool carryStream = st->active != 0 && st->ap().nowPlayingId != 0;
    const Channel carry = carryStream ? st->ap().nowPlaying : Channel{};
    // Tear the extra panes down ASYNCHRONOUSLY. libVLC 3.x stop()/release() block for seconds on a
    // stuck IPTV feed; doing that synchronously here (player.shutdown()) froze the UI thread — an
    // AppHang — when entering/leaving 2x2 or PIP with flaky streams. beginTeardown() hands the
    // blocking stop to a reaper thread and the pane parks (hidden) in dyingPanes until reaped.
    while (static_cast<int>(st->panes.size()) > 1) {
        auto p = std::move(st->panes.back());
        st->panes.pop_back();
        p->player.beginTeardown();                  // non-blocking; the blocking stop runs off-thread
        if (p->hwnd) ShowWindow(p->hwnd, SW_HIDE);   // hide until its vout is gone + we DestroyWindow it
        st->dyingPanes.push_back(std::move(p));
    }
    st->active = 0;
    st->viewMode = mode;
    st->pipMoved = false;  // a fresh PIP starts in the default corner (until the user drags it)
    if (mode == ViewMode::Split)
        for (int i = 1; i < 4; ++i) addPane(st->hwnd, st, i);        // three more tiles -> 2x2
    else if (mode == ViewMode::Pip)
        addPane(st->hwnd, st, 1, /*floating=*/true);                 // the floating PIP popup
    // Replay the carried selection into pane 0 (st->active is 0 now, so it plays audible) unless
    // pane 0 already has it. This is what makes "the selected stream survives" true.
    if (carryStream && st->panes[0]->nowPlayingId != carry.id)
        playChannelInPane(st, carry, 0);
    setActivePane(st, 0);
    layout(st->hwnd, st);
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

std::wstring recordingsDir() {
    PWSTR vids = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids)))
        dir = std::filesystem::path(vids) / L"RabbitEars";
    if (vids) CoTaskMemFree(vids);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"RabbitEars";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

std::wstring recordingPath(const std::wstring& channelName, const std::wstring& ext) {
    std::wstring name = channelName;
    for (wchar_t& ch : name)
        if (ch < 0x20 || wcschr(L"\\/:*?\"<>|'{},", ch)) ch = L'_';  // filename- + sout-safe
    if (name.empty()) name = L"channel";
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    swprintf_s(ts, L"%04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    return recordingsDir() + L"\\" + name + L" - " + ts + ext;
}

void onToggleRecord(AppState* st) {
    if (st->activeScheduleId != 0) {  // the recorder is owned by a scheduled recording
        setStatus(st, L"A scheduled recording is in progress — manage it in Scheduled Recordings.");
        return;
    }
    if (st->ap().player.isRecording()) {
        const std::wstring file = st->ap().player.recordingFile();
        st->ap().player.stopRecording();
        SetWindowTextW(st->btnRec, kGlyphRecord);
        setStatus(st, L"Recording saved: " + file);
        return;
    }
    if (st->ap().nowPlaying.id == 0) {
        setStatus(st, L"Play a channel first, then Record.");
        return;
    }
    const std::wstring ext = (st->recFormat == L"mkv") ? L".mkv" : L".ts";
    const std::string mux = (st->recFormat == L"mkv") ? "mkv" : "ts";
    const std::wstring path = recordingPath(st->ap().nowPlaying.name, ext);
    if (st->ap().player.startRecording(st->ap().nowPlaying.streamUrl, st->ap().nowPlaying.userAgent,
                                  st->ap().nowPlaying.referrer, path, mux)) {
        SetWindowTextW(st->btnRec, kGlyphStop);
        setStatus(st, L"● Recording " + st->ap().nowPlaying.name + L"  →  " + path);
    }
}

// The recording scheduler tick (~30s): decide via the pure planScheduler() core, then
// apply — driving the single shared recorder and writing status back to the DB. Runs
// only while the app is open (a schedule whose window passed while closed is marked
// Missed on the next tick). Guards against the manual Record button via activeScheduleId.
void onSchedulerTick(AppState* st) {
    reapDyingPanes(st, /*force=*/false);  // backstop: reap finished mode-switch panes (~30s tick)
    auto schedules = st->db.listSchedules();
    if (schedules.empty()) return;
    // One-time startup reconcile: a schedule still marked Recording is stale (a prior
    // session was closed mid-record — nothing is actually recording now), so reset it to
    // Pending and let planScheduler resume it (if still in window) or miss it.
    if (!st->schedulerReconciled) {
        st->schedulerReconciled = true;
        bool changed = false;
        for (const auto& s : schedules)
            if (s.status == ScheduleStatus::Recording) {
                st->db.updateScheduleStatus(s.id, ScheduleStatus::Pending);
                changed = true;
            }
        if (changed) schedules = st->db.listSchedules();
    }
    const long long now = static_cast<long long>(time(nullptr));
    // "Manual" recording = the recorder is busy but no schedule owns it.
    const bool manualRecording = st->ap().player.isRecording() && st->activeScheduleId == 0;
    const SchedulerPlan plan = planScheduler(schedules, now, manualRecording);

    for (long long id : plan.stop) {
        st->ap().player.stopRecording();
        st->db.updateScheduleStatus(id, ScheduleStatus::Done);
        if (st->activeScheduleId == id) st->activeScheduleId = 0;
        diag::info(L"scheduled recording finished (id " + std::to_wstring(id) + L")");
        setStatus(st, L"Scheduled recording saved.");
    }
    for (long long id : plan.miss) {
        st->db.updateScheduleStatus(id, ScheduleStatus::Missed);
        diag::warn(L"scheduled recording missed (id " + std::to_wstring(id) + L")");
    }
    for (long long id : plan.start) {  // planScheduler yields at most one
        const ScheduledRecording* s = nullptr;
        for (const auto& x : schedules)
            if (x.id == id) { s = &x; break; }
        if (!s) continue;
        const std::wstring ext = (s->mux == L"mkv") ? L".mkv" : L".ts";
        const std::string mux = (s->mux == L"mkv") ? "mkv" : "ts";
        const std::wstring path = recordingPath(s->channelName, ext);
        if (st->ap().player.startRecording(s->streamUrl, s->userAgent, s->referrer, path, mux)) {
            st->activeScheduleId = id;
            st->db.updateScheduleStatus(id, ScheduleStatus::Recording, path);
            diag::info(L"scheduled recording started: " + s->channelName + L" -> " + path);
            setStatus(st, L"● Recording (scheduled) " + s->channelName);
        } else {
            st->db.updateScheduleStatus(id, ScheduleStatus::Failed);
            diag::error(L"scheduled recording failed to start (id " + std::to_wstring(id) + L")");
        }
    }
}

// Schedule a recording for a guide programme: resolve its tvg-id to a recordable stream,
// store the (self-contained) schedule, nudge the scheduler in case it is already airing,
// and confirm. Called from the TV Guide's right-click "Schedule recording".
void scheduleFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    std::optional<Channel> ch;
    if (!channelId.empty()) ch = st->db.channelByTvgId(channelId);
    if (!ch) {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Channel not found",
                       L"Couldn't match \"" + channelName + L"\" to a playable channel in your "
                       L"library (its tvg-id isn't in an enabled playlist).");
        return;
    }
    ScheduledRecording s;
    s.channelId = channelId;
    s.channelName = ch->name.empty() ? channelName : ch->name;
    s.streamUrl = ch->streamUrl;
    s.userAgent = ch->userAgent;
    s.referrer = ch->referrer;
    s.title = title;
    s.startUtc = startUtc;
    s.stopUtc = stopUtc;
    s.mux = st->recFormat;  // the app's current TS/MKV setting
    s.createdAt = static_cast<long long>(time(nullptr));
    if (st->db.addSchedule(s) > 0) {
        onSchedulerTick(st);  // start immediately if the programme is already on air
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Recording scheduled",
                       title + L"\r\n" + s.channelName +
                           L"\r\n\r\nThe app must be running at the scheduled time.");
    } else {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Could not schedule",
                       L"The recording could not be saved.");
    }
}

// Settings ▸ Scheduled Recordings… — the manager (list + New/Cancel/Delete). The host
// callbacks own the recorder + DB so cancel/delete stop an active recording, and New
// opens scheduleDialog over the manager and stores + nudges the scheduler.
void onManageSchedules(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    ScheduleManagerCallbacks cb;
    cb.list = [st] { return st->db.listSchedules(); };
    cb.cancel = [st](long long id) {
        if (st->activeScheduleId == id) {
            st->ap().player.stopRecording();
            st->activeScheduleId = 0;
        }
        st->db.updateScheduleStatus(id, ScheduleStatus::Cancelled);
    };
    cb.remove = [st](long long id) {
        if (st->activeScheduleId == id) {
            st->ap().player.stopRecording();
            st->activeScheduleId = 0;
        }
        st->db.deleteSchedule(id);
    };
    cb.addNew = [st](HWND owner) {
        HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
        ScheduledRecording d;
        if (scheduleDialog(owner, hi, st->dpi, st->db.allChannels(), d)) {
            d.mux = st->recFormat;
            d.createdAt = static_cast<long long>(time(nullptr));
            if (st->db.addSchedule(d) > 0) onSchedulerTick(st);  // start now if already airing
        }
    };
    manageSchedules(st->hwnd, hInst, st->dpi, cb);
}

// Serialize the category include-set as a newline-joined list. Group titles come
// from a single M3U line so they never contain a newline; an empty stored value
// means the filter is off (show everything).
std::wstring joinCategories(const std::set<std::wstring>& s) {
    std::wstring out;
    for (const std::wstring& g : s) {
        if (!out.empty()) out += L'\n';
        out += g;
    }
    return out;
}

std::set<std::wstring> splitCategories(const std::wstring& s) {
    std::set<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : s) {
        if (ch == L'\n') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

// Settings → Categories…: a checklist over the distinct group titles. The include
// set is normalized so "all checked" and "none checked" both mean no restriction.
void onCategories(AppState* st) {
    std::vector<std::wstring> groups = st->db.listGroups();
    if (groups.empty()) {
        setStatus(st, L"No categories to filter — this library has no group titles.");
        showInfoDialog(st->hwnd,
                       reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                       st->dpi, L"Categories", L"No categories to filter",
                       L"The channels in your library have no group titles, so there are no "
                       L"categories to include or exclude.\r\n\r\n"
                       L"Add a playlist whose #EXTINF lines carry group-title tags to use this "
                       L"filter.\r\n");
        return;
    }
    std::set<std::wstring> checked;
    if (st->categoryActive)
        checked = st->categories;
    else
        checked.insert(groups.begin(), groups.end());  // all checked == no restriction

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (!chooseCategories(st->hwnd, hInst, st->dpi, groups, checked))
        return;  // cancelled — leave the current filter untouched

    const std::set<std::wstring> allSet(groups.begin(), groups.end());
    if (checked.empty() || checked == allSet) {
        st->categoryActive = false;
        st->categories.clear();
        st->db.setSetting(L"category_filter", L"");
    } else {
        st->categoryActive = true;
        st->categories = std::move(checked);
        st->db.setSetting(L"category_filter", joinCategories(st->categories));
    }
    loadForFilter(st);  // re-apply to the current nav view (mirrors Hide unavailable)
}

// Start/stop the audio spectrum tap to match the spectrum meter's visibility (no
// point capturing audio nobody's watching). The tap is read-only WASAPI loopback,
// so this never affects playback; on failure it just delivers nothing.
void syncSpectrumTap(AppState* st) {
    if (st->showSpectrum && st->meterSpectrum) {
        if (!st->spectrumTap.running()) {
            HWND m = st->meterSpectrum;
            st->spectrumTap.start(
                [m](const float* bands) { miniMeterPushSpectrum(m, bands, SpectrumTap::kBands); });
        }
    } else {
        st->spectrumTap.stop();
    }
}

// Clear the stat-driven mini meters back to idle (the spectrum decays on its own
// once the audio goes silent).
void resetStatMeters(AppState* st) {
    miniMeterReset(st->meterSignal);
    miniMeterReset(st->meterBitrate);
    miniMeterReset(st->meterFrames);
}

// Settings → Meters → Setup…: the per-meter look + palette dialog. Seeds it from the
// live meters, then on OK applies + persists (style, colours, enable) for each.
void onMeters(AppState* st) {
    HWND m[4] = {st->meterSpectrum, st->meterSignal, st->meterBitrate, st->meterFrames};
    const bool en[4] = {st->showSpectrum, st->showSignal, st->showBitrate, st->showFrames};
    MeterConfig cfg[4];
    for (int r = 0; r < 4; ++r) {
        cfg[r].enabled = en[r];
        cfg[r].style = miniMeterStyle(m[r]);
        cfg[r].palette = miniMeterPalette(m[r]);
        cfg[r].tuning = miniMeterTuning(m[r]);
    }
    // The data-flow (buffer) meter's current visible state (persisted as buffer_hidden).
    auto bh = st->db.getSetting(L"buffer_hidden");
    bool dataFlowOn = !(bh && *bh == L"1");
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (!chooseMeters(st->hwnd, hInst, st->dpi, cfg, dataFlowOn)) return;  // Cancel — no change
    bufferMeterSetHidden(st->bufferMeter, !dataFlowOn);
    st->db.setSetting(L"buffer_hidden", dataFlowOn ? L"0" : L"1");

    static const wchar_t* key[4] = {L"spectrum", L"signal", L"bitrate", L"frames"};
    st->showSpectrum = cfg[0].enabled;
    st->showSignal = cfg[1].enabled;
    st->showBitrate = cfg[2].enabled;
    st->showFrames = cfg[3].enabled;
    for (int r = 0; r < 4; ++r) {
        miniMeterSetStyle(m[r], cfg[r].style);
        miniMeterSetPalette(m[r], cfg[r].palette);
        miniMeterSetTuning(m[r], cfg[r].tuning);
        st->db.setSetting(std::wstring(L"meter_") + key[r], cfg[r].enabled ? L"1" : L"0");
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_style",
                          meterStyleToString(cfg[r].style));
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_colors",
                          meterPaletteToString(cfg[r].palette));
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_knobs",
                          meterTuningToString(cfg[r].tuning));
    }
    syncSpectrumTap(st);   // enabling/disabling spectrum starts/stops the capture tap
    layout(st->hwnd, st);  // show/hide meters per the new enables
}

// Command-bar Settings menu: Open File, About, recording format, and view toggles.
void showSettingsMenu(HWND hwnd, AppState* st, const RECT& anchor) {
    HMENU fmt = CreatePopupMenu();
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? 0u : MF_CHECKED), ID_FMT_TS,
                L"MPEG-TS  (.ts)");
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? MF_CHECKED : 0u), ID_FMT_MKV,
                L"Matroska  (.mkv)");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN_FILE, L"Open File…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), L"Recording format");
    AppendMenuW(m, MF_STRING | (st->hideDead ? MF_CHECKED : 0u), ID_HIDE_DEAD,
                L"Hide unavailable channels");
    std::wstring catLabel = L"Categories…";
    if (st->categoryActive) catLabel += L"  (" + std::to_wstring(st->categories.size()) + L")";
    AppendMenuW(m, MF_STRING | (st->categoryActive ? MF_CHECKED : 0u), ID_CATEGORIES,
                catLabel.c_str());
    AppendMenuW(m, MF_STRING, ID_EPG_GUIDE, L"TV Guide");
    AppendMenuW(m, MF_STRING, ID_EPG_REFRESH, L"Refresh Guide…");
    AppendMenuW(m, MF_STRING, ID_SCHEDULES, L"Scheduled Recordings…");

    // Meters… opens the full setup dialog (per-meter enable + look + colours + the data-flow
    // row live there now — the old inline quick-toggle checkboxes were redundant).
    AppendMenuW(m, MF_STRING, ID_METERS_SETUP, L"Meters…");
    AppendMenuW(m, MF_STRING | (st->videoOnly ? MF_CHECKED : 0u), ID_VIDEO_ONLY,
                L"Video only\tCtrl+Shift+V");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Single ? MF_CHECKED : 0u),
                ID_VIEW_SINGLE, L"Single");
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Split ? MF_CHECKED : 0u),
                ID_VIEW_SPLIT, L"Split (2×2)");
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Pip ? MF_CHECKED : 0u),
                ID_VIEW_PIP, L"Picture-in-picture");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"View");

    HMENU layoutMenu = CreatePopupMenu();
    AppendMenuW(layoutMenu, MF_STRING, ID_LAYOUT_RESET, L"Reset to default");
    AppendMenuW(layoutMenu, MF_SEPARATOR, 0, nullptr);
    const struct { const wchar_t* name; Panel p; } dockPanels[] = {
        {L"Move sidebar", Panel::Nav}, {L"Move video", Panel::Video}, {L"Move channels", Panel::Grid}};
    const wchar_t* dockSides[] = {L"To left", L"To right", L"To top", L"To bottom"};
    for (const auto& pn : dockPanels) {
        HMENU sub = CreatePopupMenu();
        for (int s = 0; s < 4; ++s)
            AppendMenuW(sub, MF_STRING, ID_DOCK_BASE + static_cast<int>(pn.p) * 4 + s, dockSides[s]);
        AppendMenuW(layoutMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), pn.name);
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"Layout");

#ifdef RABBITEARS_THEME_ENGINE
    HMENU themeMenu = CreatePopupMenu();
    const std::string& skinSel = activeSkinSelection();
    AppendMenuW(themeMenu, MF_STRING | (skinSel == "system" ? MF_CHECKED : 0u), ID_THEME_SYSTEM,
                L"Follow System");
    AppendMenuW(themeMenu, MF_SEPARATOR, 0, nullptr);
    const auto& skins = builtinSkins();  // one item per registered skin (auto-grows in Phase 4)
    for (size_t i = 0; i < skins.size(); ++i)
        AppendMenuW(themeMenu, MF_STRING | (skinSel == skins[i].id ? MF_CHECKED : 0u),
                    ID_THEME_SKIN_BASE + static_cast<int>(i), wideFromUtf8(skins[i].name).c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"Theme");
#endif

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"About…");  // last item, ellipsis to match siblings

    POINT pt{anchor.left, anchor.bottom};
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);  // frees the submenus too
    if (cmd == ID_LAYOUT_RESET) {
        st->dock = DockLayout::makeDefault();
        applyDockChange(hwnd, st);
        return;
    }
    if (cmd >= ID_DOCK_BASE && cmd < ID_DOCK_BASE + kPanelCount * 4) {
        const int off = cmd - ID_DOCK_BASE;
        dockToEdge(st, static_cast<Panel>(off / 4), static_cast<DockSide>(off % 4));
        applyDockChange(hwnd, st);
        return;
    }
#ifdef RABBITEARS_THEME_ENGINE
    if (cmd == ID_THEME_SYSTEM) {
        setSkinSelection(hwnd, st, "system");
        return;
    }
    if (cmd >= ID_THEME_SKIN_BASE &&
        cmd < ID_THEME_SKIN_BASE + static_cast<int>(builtinSkins().size())) {
        setSkinSelection(hwnd, st, builtinSkins()[cmd - ID_THEME_SKIN_BASE].id.c_str());
        return;
    }
#endif
    switch (cmd) {
        case ID_OPEN_FILE:
            onOpenFile(st);
            break;
        case ID_ABOUT:
            showAbout(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                      st->dpi);
            break;
        case ID_FMT_TS:
            st->recFormat = L"ts";
            st->db.setSetting(L"rec_format", L"ts");
            break;
        case ID_FMT_MKV:
            st->recFormat = L"mkv";
            st->db.setSetting(L"rec_format", L"mkv");
            break;
        case ID_HIDE_DEAD:
            st->hideDead = !st->hideDead;
            st->db.setSetting(L"hide_dead", st->hideDead ? L"1" : L"0");
            loadForFilter(st);
            break;
        case ID_CATEGORIES:
            onCategories(st);
            break;
        case ID_EPG_GUIDE:
            onEpgGuide(st);
            break;
        case ID_SCHEDULES:
            onManageSchedules(st);
            break;
        case ID_EPG_REFRESH:
            onEpgRefresh(st);
            break;
        case ID_METERS_SETUP:
            onMeters(st);
            break;
        case ID_VIDEO_ONLY:
            toggleVideoOnly(st);
            break;
        case ID_VIEW_SINGLE:
            applyViewMode(st, ViewMode::Single);
            break;
        case ID_VIEW_SPLIT:
            applyViewMode(st, ViewMode::Split);
            break;
        case ID_VIEW_PIP:
            applyViewMode(st, ViewMode::Pip);
            break;
    }
}


}  // namespace mw
}  // namespace rabbitears
