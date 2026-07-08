// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window — nav / filters / playlist worker / buffer / meters (split from MainWindow.cpp).
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

// ---- data ------------------------------------------------------------------

void updateCounts(AppState* st) {
    int shown = 0, total = 0;
    channelGridGetCounts(st->grid, &shown, &total);
    setStatus(st, std::to_wstring(shown) + L" channels");
}

// Apply the global view settings (currently: hide unavailable/geo-blocked). Reused
// by the nav views and global search so the toggle is consistent everywhere.
void applyChannelFilters(AppState* st, std::vector<Channel>& ch) {
    if (st->hideDead)
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [](const Channel& c) { return c.deadStatus == DeadStatus::Dead; }),
                 ch.end());
    if (st->categoryActive && !st->categories.empty())
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [st](const Channel& c) {
                                    // Uncategorized channels (blank group) can't be picked in
                                    // the Categories dialog, so never hide them behind it.
                                    return !c.groupTitle.empty() &&
                                           st->categories.find(c.groupTitle) == st->categories.end();
                                }),
                 ch.end());
}

void loadForFilter(AppState* st) {
    std::vector<Channel> ch;
    switch (st->filter.kind) {
        case ViewKind::All: ch = st->db.allChannels(); break;
        case ViewKind::Favourites: ch = st->db.favourites(); break;
        case ViewKind::Group: ch = st->db.channelsByGroup(st->filter.group); break;
        case ViewKind::Country: ch = st->db.channelsByCountry(st->filter.country); break;
        case ViewKind::Playlist: ch = st->db.channelsByPlaylist(st->filter.playlistId); break;
        case ViewKind::Guide: break;  // action node (opens the TV Guide window); loads no grid channels
    }
    applyChannelFilters(st, ch);
    channelGridSetChannels(st->grid, std::move(ch));
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    updateCounts(st);
}

HTREEITEM navInsert(HWND nav, HTREEITEM parent, const std::wstring& text, LPARAM param, bool bold) {
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    is.item.pszText = const_cast<LPWSTR>(text.c_str());
    is.item.lParam = param;
    if (bold) {
        is.item.mask |= TVIF_STATE;
        is.item.state = TVIS_BOLD;
        is.item.stateMask = TVIS_BOLD;
    }
    return TreeView_InsertItem(nav, &is);
}

// Friendly name for an ISO-3166 alpha-2 country code (from the tvg-id suffix); the
// uppercased code itself for anything not in the (common-IPTV-countries) table.
std::wstring countryLabel(const std::wstring& code) {
    struct CC { const wchar_t* code; const wchar_t* name; };
    static const CC kNames[] = {
        {L"us", L"United States"}, {L"uk", L"United Kingdom"}, {L"gb", L"United Kingdom"},
        {L"ca", L"Canada"}, {L"au", L"Australia"}, {L"nz", L"New Zealand"}, {L"ie", L"Ireland"},
        {L"de", L"Germany"}, {L"fr", L"France"}, {L"es", L"Spain"}, {L"it", L"Italy"},
        {L"pt", L"Portugal"}, {L"nl", L"Netherlands"}, {L"be", L"Belgium"}, {L"ch", L"Switzerland"},
        {L"at", L"Austria"}, {L"se", L"Sweden"}, {L"no", L"Norway"}, {L"dk", L"Denmark"},
        {L"fi", L"Finland"}, {L"pl", L"Poland"}, {L"cz", L"Czechia"}, {L"sk", L"Slovakia"},
        {L"hu", L"Hungary"}, {L"ro", L"Romania"}, {L"bg", L"Bulgaria"}, {L"gr", L"Greece"},
        {L"tr", L"Turkey"}, {L"ru", L"Russia"}, {L"ua", L"Ukraine"}, {L"rs", L"Serbia"},
        {L"hr", L"Croatia"}, {L"si", L"Slovenia"}, {L"al", L"Albania"}, {L"br", L"Brazil"},
        {L"mx", L"Mexico"}, {L"ar", L"Argentina"}, {L"cl", L"Chile"}, {L"co", L"Colombia"},
        {L"pe", L"Peru"}, {L"ve", L"Venezuela"}, {L"in", L"India"}, {L"pk", L"Pakistan"},
        {L"bd", L"Bangladesh"}, {L"cn", L"China"}, {L"jp", L"Japan"}, {L"kr", L"South Korea"},
        {L"id", L"Indonesia"}, {L"my", L"Malaysia"}, {L"sg", L"Singapore"}, {L"th", L"Thailand"},
        {L"vn", L"Vietnam"}, {L"ph", L"Philippines"}, {L"sa", L"Saudi Arabia"}, {L"ae", L"UAE"},
        {L"qa", L"Qatar"}, {L"il", L"Israel"}, {L"eg", L"Egypt"}, {L"ma", L"Morocco"},
        {L"dz", L"Algeria"}, {L"za", L"South Africa"}, {L"ng", L"Nigeria"}, {L"ke", L"Kenya"},
    };
    for (const CC& e : kNames)
        if (code == e.code) return e.name;
    std::wstring up = code;
    for (wchar_t& c : up)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
    return up;
}

void refreshNav(AppState* st) {
    st->navFilters.clear();
    TreeView_DeleteAllItems(st->nav);

    st->navFilters.push_back({ViewKind::All});
    navInsert(st->nav, TVI_ROOT, L"All Channels", 0, false);
    st->navFilters.push_back({ViewKind::Favourites});
    navInsert(st->nav, TVI_ROOT, L"★ Favourites", 1, false);
    st->navFilters.push_back({ViewKind::Guide});
    navInsert(st->nav, TVI_ROOT, L"📺 TV Guide", 2, false);  // selecting it opens the guide window

    HTREEITEM groups = navInsert(st->nav, TVI_ROOT, L"Groups", -1, true);
    for (const std::wstring& g : st->db.listGroups()) {
        st->navFilters.push_back({ViewKind::Group, g, 0});
        navInsert(st->nav, groups, g, static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    HTREEITEM countries = navInsert(st->nav, TVI_ROOT, L"Countries", -1, true);
    {
        std::vector<std::pair<std::wstring, std::wstring>> cs;  // (display name, code)
        for (const std::wstring& cc : st->db.listCountries()) cs.emplace_back(countryLabel(cc), cc);
        std::sort(cs.begin(), cs.end());  // alphabetical by name
        for (const auto& [label, cc] : cs) {
            st->navFilters.push_back({ViewKind::Country, L"", 0, cc});
            navInsert(st->nav, countries, label, static_cast<LPARAM>(st->navFilters.size() - 1), false);
        }
    }
    HTREEITEM playlists = navInsert(st->nav, TVI_ROOT, L"Playlists", -1, true);
    for (const Playlist& p : st->db.listPlaylists()) {
        st->navFilters.push_back({ViewKind::Playlist, L"", p.id});
        navInsert(st->nav, playlists, p.name + L" (" + std::to_wstring(p.channelCount) + L")",
                  static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    TreeView_Expand(st->nav, playlists, TVE_EXPAND);
}

void resetStatMeters(AppState* st);  // defined below — clear the stat meters on switch

// Play `c` into pane `idx`. When idx is the active pane it also drives the shared chrome (grid
// now-playing highlight, meters, status, last-channel); a background pane (e.g. the PIP) just loads
// and plays — muted, since only the active pane is audible (click it to hear it).
void playChannelInPane(AppState* st, const Channel& c, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->panes.size())) return;
    VideoPane& p = *st->panes[idx];
    diag::info(L"play pane " + std::to_wstring(idx) + L" #" + std::to_wstring(c.id) + L" \"" + c.name +
               L"\" ua=[" + c.userAgent + L"] ref=[" + c.referrer + L"]");
    if (p.player.isReady()) p.player.play(c.streamUrl, c.userAgent, c.referrer);
    // Only the active pane is audible: a background/non-active tile plays with its audio track
    // deselected (setMuted) so it stays silent even across adaptive quality switches; the active
    // tile keeps its track and gets the slider volume.
    const int vol = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
    p.player.setMuted(idx != st->active);
    if (idx == st->active) p.player.setVolume(vol);
    p.nowPlayingId = c.id;
    p.nowPlayingName = c.name;
    p.nowPlaying = c;
    if (idx == st->active) {
        channelGridSetNowPlaying(st->grid, c.id);
        st->db.setSetting(L"last_channel_id", std::to_wstring(c.id));
        bufferMeterSetHealth(st->bufferMeter, 15);
        resetStatMeters(st);  // clear signal/bitrate/frames so switching to a dead/stalled stream
                              // can't leave the previous channel's readings frozen on the meters
        setStatus(st, L"Opening: " + c.name);
    } else {
        setStatus(st, L"PIP: " + c.name);
    }
}

void playChannel(AppState* st, const Channel& c) { playChannelInPane(st, c, st->active); }

std::wstring bufLabelText(int ms) {
    wchar_t b[24];
    swprintf_s(b, L"Buffer %.1f s", ms / 1000.0);
    return b;
}

// Snap + apply the network buffer size, persist it, sync the slider/label, and
// (optionally) re-buffer the current stream so the change takes effect immediately.
void setBufferMs(AppState* st, int ms, bool replay) {
    ms = std::clamp((ms + kBufStepMs / 2) / kBufStepMs * kBufStepMs, kBufMinMs, kBufMaxMs);
    st->ap().player.setNetworkCaching(ms);
    st->db.setSetting(L"buffer_ms", std::to_wstring(ms));
    if (st->bufBar) SendMessageW(st->bufBar, TBM_SETPOS, TRUE, ms / kBufStepMs);
    if (st->bufLabel) SetWindowTextW(st->bufLabel, bufLabelText(ms).c_str());
    if (replay && st->ap().player.isPlaying() && st->ap().nowPlaying.id != 0) playChannel(st, st->ap().nowPlaying);
}

std::wstring nameFromSource(const std::wstring& src, bool isUrl) {
    size_t slash = src.find_last_of(isUrl ? L"/" : L"\\/");
    std::wstring n = (slash == std::wstring::npos) ? src : src.substr(slash + 1);
    if (n.empty()) n = src;
    return n;
}

void startPlaylistWorker(AppState* st, const std::wstring& source, bool isUrl,
                         const std::wstring& name) {
    st->busy = true;
    setStatus(st, isUrl ? L"Downloading playlist…" : L"Loading playlist…");
    diag::info((isUrl ? L"playlist download start: " : L"playlist load start: ") + source);
    HWND hwnd = st->hwnd;
    std::thread([hwnd, source, isUrl, name]() {
        auto* res = new PlaylistResult();
        res->isUrl = isUrl;
        res->source = source;
        res->name = name;
        if (isUrl) {
            std::string bytes;
            // 30 s per-phase timeout so a stalled connection can't hang the worker
            // forever (which would latch `busy` and leave no feedback).
            if (httpGet(source, bytes, res->error, 30000)) {
                diag::info(L"downloaded " + std::to_wstring(bytes.size()) + L" bytes");
                res->doc = parseM3u(bytes);
                res->ok = true;
                diag::info(L"parsed " + std::to_wstring(res->doc.channels.size()) + L" channels");
            } else {
                diag::error(L"download failed from " + source + L": " + res->error);
            }
        } else {
            std::wstring err;
            res->doc = parseM3uFile(source, &err);
            res->error = err;
            res->ok = err.empty();
            if (res->ok)
                diag::info(L"parsed " + std::to_wstring(res->doc.channels.size()) + L" channels");
            else
                diag::error(L"file load failed: " + err);
        }
        res->parsed = static_cast<int>(res->doc.channels.size());
        std::set<std::wstring> grp;
        for (const auto& c : res->doc.channels)
            if (!c.groupTitle.empty()) grp.insert(c.groupTitle);
        res->groups = static_cast<int>(grp.size());
        PostMessageW(hwnd, WM_APP_PLAYLIST_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}


}  // namespace mw
}  // namespace rabbitears
