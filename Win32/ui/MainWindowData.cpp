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
#include "ui/Tr.h"
#include "ui/VideoGrid.h"
#include "ui/VlcEngine.h"
#include "ui/VlcPlayer.h"
#include "ui/VodSync.h"

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
    setStatus(st, trf(i18n::StringId::StatusChannelCount, { std::to_wstring(shown) }));
}

// Apply the global view settings (currently: hide unavailable/geo-blocked). Reused
// by the nav views and global search so the toggle is consistent everywhere.
// The grid's view settings as a DAO filter. These used to be applied in C++ over the returned
// vector; they moved into SQL when the row cap arrived, because a cap composed with a post-query
// filter yields "the matches among the first N" rather than "the first N matches" — see
// Database::GridFilter. The exemptions for VOD rows and blank groups are unchanged and are
// documented there.
//
// `limit` is asked for as kMaxGridRows + 1 on purpose: getting that many back is what tells the
// caller the library has MORE, exactly, instead of guessing from `size() == limit` — which cannot
// distinguish a truncated view from one that happens to hold exactly kMaxGridRows rows.
Database::GridFilter gridFilter(AppState* st, bool capped) {
    Database::GridFilter g;
    g.hideDead = st->hideDead;
    if (st->categoryActive && !st->categories.empty())
        g.categories.assign(st->categories.begin(), st->categories.end());
    if (capped) g.limit = kMaxGridRows + 1;
    return g;
}

// Trim the +1 probe row and report whether it was there. Keeping this beside gridFilter() so the
// two halves of the trick cannot drift apart.
bool trimToGridCap(std::vector<Channel>& ch) {
    if (static_cast<int>(ch.size()) <= kMaxGridRows) return false;
    ch.resize(kMaxGridRows);
    return true;
}

void cancelSearchDebounce(AppState* st) {
    st->searchPending = false;
    if (st->hwnd) KillTimer(st->hwnd, kSearchDebounceTimer);
}

// The search box's EN_CHANGE only ARMS a timer; this is the work it defers. Deliberately global
// (any name/group/tvg match across the whole library), not scoped to the current nav view.
//
// Known and accepted: the closing updateCounts() now lands ~200 ms after the keystroke instead of
// during it, so it can overwrite a status message written in that window (a player event, say).
// That race already existed — the status line is one shared slot and this was always its last
// writer for the final keystroke of a burst — the debounce only widens the window, and the same
// keystroke previously froze the UI for longer than the window it opens. The sticky truncation
// notice is unaffected: setStatus re-appends it from st->gridTruncated on every write.
void applySearch(AppState* st) {
    cancelSearchDebounce(st);  // consuming the request, whether it came from the tick or directly
    wchar_t buf[256] = L"";
    GetWindowTextW(st->search, buf, ARRAYSIZE(buf));
    const std::wstring q = buf;
    if (q.empty()) {
        loadForFilter(st);  // back to the current nav view
        return;
    }
    std::vector<Channel> hits = st->db.searchChannels(q, gridFilter(st, /*capped=*/true));
    st->gridTruncated = trimToGridCap(hits);
    channelGridSetChannels(st->grid, std::move(hits));
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    updateCounts(st);
}

void loadForFilter(AppState* st) {
    // Whatever brought us here — a nav click, a view toggle, a finished sweep — is a newer answer
    // to "what should the grid show" than a keystroke that has not been queried yet. Cancelling
    // here rather than at each call site means a future caller cannot forget to. It also preserves
    // today's semantics exactly: a synchronous search was already clobbered by these same paths.
    cancelSearchDebounce(st);
    std::vector<Channel> ch;
    const Database::GridFilter g = gridFilter(st, /*capped=*/true);
    switch (st->filter.kind) {
        case ViewKind::All: ch = st->db.allChannels(g); break;
        case ViewKind::Favourites: ch = st->db.favourites(g); break;
        case ViewKind::Group: ch = st->db.channelsByGroup(st->filter.group, g); break;
        case ViewKind::Country: ch = st->db.channelsByCountry(st->filter.country, g); break;
        case ViewKind::Playlist: ch = st->db.channelsByPlaylist(st->filter.playlistId, g); break;
        case ViewKind::Guide: break;  // action node (opens the TV Guide window); loads no grid channels
        // The Movies ROOT loads nothing on purpose — see the ViewKind comment. It still falls
        // through to the clear-the-grid path below so the view has a defined state rather than
        // the previous nav node's rows left sitting there.
        case ViewKind::Movies: break;
        case ViewKind::MovieGroup: ch = st->db.moviesByGroup(st->filter.group, g); break;
    }
    st->gridTruncated = trimToGridCap(ch);
    channelGridSetChannels(st->grid, std::move(ch));
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    updateCounts(st);
    // ...and say WHY it is empty. updateCounts would otherwise report "0 channels", which reads
    // as "your movies are gone" on the one node whose whole job is to point at its children.
    if (st->filter.kind == ViewKind::Movies)
        setStatus(st, tr(i18n::StringId::StatusMoviesPickCategory));
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
    struct CC { const wchar_t* code; i18n::StringId name; };
    static const CC kNames[] = {
        {L"us", i18n::StringId::CountryUnitedStates}, {L"uk", i18n::StringId::CountryUnitedKingdom}, {L"gb", i18n::StringId::CountryUnitedKingdom},
        {L"ca", i18n::StringId::CountryCanada}, {L"au", i18n::StringId::CountryAustralia}, {L"nz", i18n::StringId::CountryNewZealand}, {L"ie", i18n::StringId::CountryIreland},
        {L"de", i18n::StringId::CountryGermany}, {L"fr", i18n::StringId::CountryFrance}, {L"es", i18n::StringId::CountrySpain}, {L"it", i18n::StringId::CountryItaly},
        {L"pt", i18n::StringId::CountryPortugal}, {L"nl", i18n::StringId::CountryNetherlands}, {L"be", i18n::StringId::CountryBelgium}, {L"ch", i18n::StringId::CountrySwitzerland},
        {L"at", i18n::StringId::CountryAustria}, {L"se", i18n::StringId::CountrySweden}, {L"no", i18n::StringId::CountryNorway}, {L"dk", i18n::StringId::CountryDenmark},
        {L"fi", i18n::StringId::CountryFinland}, {L"pl", i18n::StringId::CountryPoland}, {L"cz", i18n::StringId::CountryCzechia}, {L"sk", i18n::StringId::CountrySlovakia},
        {L"hu", i18n::StringId::CountryHungary}, {L"ro", i18n::StringId::CountryRomania}, {L"bg", i18n::StringId::CountryBulgaria}, {L"gr", i18n::StringId::CountryGreece},
        {L"tr", i18n::StringId::CountryTurkey}, {L"ru", i18n::StringId::CountryRussia}, {L"ua", i18n::StringId::CountryUkraine}, {L"rs", i18n::StringId::CountrySerbia},
        {L"hr", i18n::StringId::CountryCroatia}, {L"si", i18n::StringId::CountrySlovenia}, {L"al", i18n::StringId::CountryAlbania}, {L"br", i18n::StringId::CountryBrazil},
        {L"mx", i18n::StringId::CountryMexico}, {L"ar", i18n::StringId::CountryArgentina}, {L"cl", i18n::StringId::CountryChile}, {L"co", i18n::StringId::CountryColombia},
        {L"pe", i18n::StringId::CountryPeru}, {L"ve", i18n::StringId::CountryVenezuela}, {L"in", i18n::StringId::CountryIndia}, {L"pk", i18n::StringId::CountryPakistan},
        {L"bd", i18n::StringId::CountryBangladesh}, {L"cn", i18n::StringId::CountryChina}, {L"jp", i18n::StringId::CountryJapan}, {L"kr", i18n::StringId::CountrySouthKorea},
        {L"id", i18n::StringId::CountryIndonesia}, {L"my", i18n::StringId::CountryMalaysia}, {L"sg", i18n::StringId::CountrySingapore}, {L"th", i18n::StringId::CountryThailand},
        {L"vn", i18n::StringId::CountryVietnam}, {L"ph", i18n::StringId::CountryPhilippines}, {L"sa", i18n::StringId::CountrySaudiArabia}, {L"ae", i18n::StringId::CountryUae},
        {L"qa", i18n::StringId::CountryQatar}, {L"il", i18n::StringId::CountryIsrael}, {L"eg", i18n::StringId::CountryEgypt}, {L"ma", i18n::StringId::CountryMorocco},
        {L"dz", i18n::StringId::CountryAlgeria}, {L"za", i18n::StringId::CountrySouthAfrica}, {L"ng", i18n::StringId::CountryNigeria}, {L"ke", i18n::StringId::CountryKenya},
    };
    for (const CC& e : kNames)
        if (code == e.code) return tr(e.name);
    std::wstring up = code;
    for (wchar_t& c : up)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
    return up;
}

void refreshNav(AppState* st) {
    st->navFilters.clear();
    st->navMovies = nullptr;  // cleared BEFORE the delete: the old HTREEITEM dies with it
    TreeView_DeleteAllItems(st->nav);

    st->navFilters.push_back({ViewKind::All});
    navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavAllChannels), 0, false);
    st->navFilters.push_back({ViewKind::Favourites});
    navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavFavourites), 1, false);
    st->navFilters.push_back({ViewKind::Guide});
    navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavTvGuide), 2, false);  // selecting it opens the guide window

    // 🎬 Movies — the VOD root, and the ONLY entry point to the movie library (`listGroups()`
    // is live-only, so VOD categories are not siblings in the Groups tree). Omitted entirely
    // when there are no movies, which is what keeps the sidebar byte-identical for every
    // existing live-TV-only user; one extra ~0.07 ms query is the whole cost of asking.
    //
    // Unlike Groups/Countries/Playlists this root IS selectable (lParam is a real filter index,
    // not -1) so that clicking it clears the grid and explains itself, instead of leaving the
    // previous view's rows on screen under a heading that says "Movies".
    if (const std::vector<std::wstring> vodGroups = st->db.listVodGroups(); !vodGroups.empty()) {
        st->navFilters.push_back({ViewKind::Movies});
        HTREEITEM movies = navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavMovies),
                                     static_cast<LPARAM>(st->navFilters.size() - 1), true);
        st->navMovies = movies;
        for (const std::wstring& g : vodGroups) {
            st->navFilters.push_back({ViewKind::MovieGroup, g, 0});
            navInsert(st->nav, movies, g, static_cast<LPARAM>(st->navFilters.size() - 1), false);
        }
    }

    HTREEITEM groups = navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavGroups), -1, true);
    for (const std::wstring& g : st->db.listGroups()) {
        st->navFilters.push_back({ViewKind::Group, g, 0});
        navInsert(st->nav, groups, g, static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    HTREEITEM countries = navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavCountries), -1, true);
    {
        std::vector<std::pair<std::wstring, std::wstring>> cs;  // (display name, code)
        for (const std::wstring& cc : st->db.listCountries()) cs.emplace_back(countryLabel(cc), cc);
        std::sort(cs.begin(), cs.end());  // alphabetical by name
        for (const auto& [label, cc] : cs) {
            st->navFilters.push_back({ViewKind::Country, L"", 0, cc});
            navInsert(st->nav, countries, label, static_cast<LPARAM>(st->navFilters.size() - 1), false);
        }
    }
    HTREEITEM playlists = navInsert(st->nav, TVI_ROOT, tr(i18n::StringId::NavPlaylists), -1, true);
    for (const Playlist& p : st->db.listPlaylists()) {
        st->navFilters.push_back({ViewKind::Playlist, L"", p.id});
        navInsert(st->nav, playlists,
                  trf(i18n::StringId::NavPlaylistNameCount, { p.name, std::to_wstring(p.channelCount) }),
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
    // 🔴 max_connections:1 — the sync gate cannot be start-only. startVodSync() refuses while
    // anything is playing or recording, but nothing stopped the user pressing play two seconds
    // into a ~10 s catalogue download, and on a one-connection line that is the sync kicking the
    // stream the user just asked for. The user's playback always wins, so the sync stands down.
    // It is a soft stop: httpGet has no cancellation handle, so an in-flight body still finishes
    // — but no further request is issued and nothing is written.
    cancelVodSync();
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
        // A new stream in the active pane invalidates any pending seek: the old target means
        // nothing here, and relying on the visibility transition to clear it is a race — the
        // worker republishes a length every 250 ms, so VOD->VOD may never flip visibility.
        clearSeekGesture(st);
        setStatus(st, trf(i18n::StringId::StatusOpening, { c.name }));
    } else {
        setStatus(st, trf(i18n::StringId::StatusPipChannel, { c.name }));
    }
}

void playChannel(AppState* st, const Channel& c) { playChannelInPane(st, c, st->active); }

std::wstring bufLabelText(int ms) {
    wchar_t b[24];
    swprintf_s(b, tr(i18n::StringId::TransportBufferSeconds).c_str(), ms / 1000.0);
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

std::wstring formatHms(long long ms) {
    if (ms < 0) ms = 0;
    const long long total = ms / 1000;
    const long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    wchar_t b[24];
    if (h > 0) swprintf_s(b, L"%lld:%02lld:%02lld", h, m, s);
    else       swprintf_s(b, L"%lld:%02lld", m, s);
    return b;
}

// Drive the scrub bar + "12:34 / 1:45:07" readout from the ACTIVE pane. Returns true when
// visibility changed, so the caller can re-run layout() — the pair takes strip width away
// from the meter tray, and only the layout pass knows whether there is room.
//
// Three sources compete for the displayed position, in priority order:
//   1. the user's thumb, while dragging — the drag must never fight the player;
//   2. the post-seek latch — libVLC reports the PRE-seek time for a beat after a seek,
//      which without this reads as the thumb snapping back and then jumping forward;
//   3. the player's published time, the normal case.
// Clear a half-finished gesture. Must run on EVERY stream change, not just when the bar's
// visibility flips: switching between two seekable panes leaves `want` unchanged, so the
// visibility-transition path never fires and pane A's seek target would be applied to
// pane B — sending its thumb to the end of a different film for up to 3 seconds.
void clearSeekGesture(AppState* st) {
    st->seekDragging = false;
    st->seekLatchMs = -1;
    st->seekTick = -1;
}

// Force the pair off and clear everything. This exists because of an asymmetry that is easy
// to miss: VlcPlayer::doStop() DETACHES its libVLC callbacks before tearing the player down,
// deliberately, so a dying stream cannot post stale events — which means an explicit Stop
// posts NO event at all, and sampleStats() has already stopped running. Nothing arrives to
// retire the bar, so the caller has to. Returns true if the strip needs to re-flow.
bool resetSeekUi(AppState* st) {
    clearSeekGesture(st);
    if (!st->seekShown) return false;
    st->seekShown = false;
    if (st->seekBar) ShowWindow(st->seekBar, SW_HIDE);
    if (st->timeLabel) ShowWindow(st->timeLabel, SW_HIDE);
    return true;
}

bool updateSeekUi(AppState* st) {
    if (!st->seekBar || !st->timeLabel) return false;
    VlcPlayer& p = st->ap().player;
    // Recover a stuck drag. seekDragging is set by every non-ENDTRACK notification and
    // cleared only by ENDTRACK; if a gesture is cancelled without one (capture stolen by a
    // system modal, the control hidden mid-drag) the bar would ignore the player forever,
    // because a "dragging" bar reads its position from the control instead of writing it.
    if (st->seekDragging && GetCapture() != st->seekBar && GetFocus() != st->seekBar)
        st->seekDragging = false;
    const long long len = p.lengthMs();
    // Both conditions matter: a live stream is not seekable, and a seekable stream with no
    // length yet (the first moments of a VOD open) has nothing to map the thumb onto.
    const bool want = p.isSeekable() && len > 0;

    const bool changed = (want != st->seekShown);
    if (changed) {
        st->seekShown = want;
        if (!want) {
            // Leaving VOD: drop the drag/latch state too, or a later stream inherits a
            // stale target and jumps on its first update.
            clearSeekGesture(st);
            ShowWindow(st->seekBar, SW_HIDE);
            ShowWindow(st->timeLabel, SW_HIDE);
        }
        // Showing is left to layout(): it decides whether the strip has room at all.
    }
    if (!want) return changed;

    long long shown = p.timeMs();
    if (st->seekDragging) {
        // The thumb is the truth while held; read the position back off the control.
        const long long tick = SendMessageW(st->seekBar, TBM_GETPOS, 0, 0);
        shown = len * tick / kSeekTicks;
    } else if (st->seekLatchMs >= 0) {
        long long drift = p.timeMs() - st->seekLatchMs;
        if (drift < 0) drift = -drift;
        // Clear the latch once the player agrees (within a tolerance wider than one
        // 250 ms sample) or the deadline passes, so a seek the demuxer silently refused
        // cannot freeze the readout forever.
        if (drift < 1500 || GetTickCount64() > st->seekLatchUntil)
            st->seekLatchMs = -1;
        else
            shown = st->seekLatchMs;
    }
    if (shown > len) shown = len;

    if (!st->seekDragging) {
        const long long tick = len > 0 ? shown * kSeekTicks / len : 0;
        // Only when it actually moved. On a 2-hour film a tick is ~7 s, so ~96% of these
        // 4 Hz updates would otherwise be a redundant repaint of a control sitting on the
        // transport strip — the same reason the label below is diffed before it is set.
        if (tick != st->seekTick) {
            st->seekTick = tick;
            SendMessageW(st->seekBar, TBM_SETPOS, TRUE, static_cast<LPARAM>(tick));
        }
    }
    const std::wstring text = formatHms(shown) + L" / " + formatHms(len);
    wchar_t cur[48] = L"";
    GetWindowTextW(st->timeLabel, cur, ARRAYSIZE(cur));
    if (text != cur) SetWindowTextW(st->timeLabel, text.c_str());  // avoid a repaint each tick
    return changed;
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
    setStatus(st, isUrl ? tr(i18n::StringId::StatusDownloadingPlaylist) : tr(i18n::StringId::StatusLoadingPlaylist));
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
