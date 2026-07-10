// SPDX-License-Identifier: GPL-3.0-or-later
// Shared internals for the RabbitEars main window (split across MainWindow*.cpp).
// Holds AppState + the local view/EPG/pane structs, the command/message/timer IDs,
// and the transport glyphs. Everything lives in namespace rabbitears::mw — a NAMED
// namespace (it was anonymous while MainWindow.cpp was one file) so the definitions
// can be split across translation units. Cross-file function prototypes get added
// here as functions move out of MainWindow.cpp.
#pragma once

#include <windows.h>
#include <commctrl.h>  // HTREEITEM (navInsert)

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/SpectrumTap.h"   // SpectrumTap (AppState)
#include "core/M3uParser.h"      // M3uDocument (PlaylistResult)
#include "core/XmltvParser.h"    // Programme (EpgFetch)
#include "db/Database.h"         // Database, Channel
#include "ui/DockLayout.h"       // DockLayout, Panel, DockSide, kPanelCount
#include "ui/VideoGrid.h"        // ViewMode
#include "ui/VlcEngine.h"        // VlcEngine
#include "ui/VlcPlayer.h"        // VlcPlayer

namespace rabbitears {
namespace mw {

// ---- window classes / messages / timers -----------------------------------
constexpr wchar_t kMainClass[] = L"RabbitEarsMain";
constexpr wchar_t kVideoClass[] = L"ReVideoSurface";
constexpr wchar_t kVoutHostClass[] = L"ReVoutHost";    // per-pane vout-host child (libVLC renders here)
constexpr wchar_t kGripClass[] = L"ReDockGrip";        // drag-to-redock grip child window
constexpr wchar_t kOverlayClass[] = L"ReDropOverlay";  // drop-zone highlight popup
constexpr UINT WM_APP_VLC = WM_APP + 1;
constexpr UINT WM_APP_PLAYLIST_DONE = WM_APP + 2;
constexpr UINT WM_APP_EPG_DONE = WM_APP + 3;  // EPG fetch/parse worker -> UI thread
constexpr UINT WM_APP_EPG_PROGRESS = WM_APP + 4;  // EPG worker progress text (heap wstring*) -> loading box
// A VlcPlayer worker (off the UI thread) that ran out of free vout hosts asks the UI thread to
// create one via SendMessageTimeout(WM_APP_MAKE_VOUT_HOST, wParam=pane HWND, lParam=pane index);
// the handler returns the new host HWND. Window create/size lives on the UI thread (Win32 affinity).
constexpr UINT WM_APP_MAKE_VOUT_HOST = WM_APP + 5;
constexpr UINT_PTR kSchedulerTimer = 0xA2;    // recording-scheduler tick (~30s; not theme-gated)

// ---- command ids ----------------------------------------------------------
constexpr int ID_ADD_URL = 2001;
constexpr int ID_OPEN_FILE = 2002;
constexpr int ID_ABOUT = 2003;
constexpr int ID_SETTINGS = 2004;   // command-bar Settings menu
constexpr int ID_FMT_TS = 2005;     // recording format: MPEG-TS
constexpr int ID_FMT_MKV = 2006;    // recording format: Matroska
constexpr int ID_HIDE_DEAD = 2007;  // hide unavailable (dead/geo-blocked) channels
constexpr int ID_CATEGORIES = 2008;  // Categories… include-filter dialog
constexpr int ID_BTN_PLAY = 2010;
constexpr int ID_BTN_STOP = 2011;
constexpr int ID_VOL = 2012;
constexpr int ID_BTN_FULL = 2013;
constexpr int ID_BUFFER = 2014;
constexpr int ID_BUF = 2015;  // buffer-size slider
constexpr int ID_BTN_REC = 2016;
constexpr int ID_SEARCH = 2020;
constexpr int ID_GRID = 2021;
constexpr int ID_NAV = 2022;
constexpr int ID_METER_SPECTRUM = 2030;  // mini-meter control ids
constexpr int ID_METER_SIGNAL = 2031;
constexpr int ID_METER_BITRATE = 2032;
constexpr int ID_METER_FRAMES = 2033;
constexpr int ID_METERS_SETUP = 2044;  // Settings → Meters… (opens the full setup dialog)
constexpr int ID_VIDEO_ONLY = 2046;    // Settings → Video only (hide all chrome; dbl-click/Esc restores)
constexpr int ID_EPG_REFRESH = 2047;   // Settings → Refresh Guide (fetch XMLTV for enabled playlists)
constexpr int ID_EPG_GUIDE = 2048;     // Settings → TV Guide (open the timeline guide window)
constexpr int ID_SCHEDULES = 2070;     // Settings → Scheduled Recordings… (recording scheduler manager)
constexpr int ID_VIEW_SINGLE = 2071;   // Settings → View: one pane (classic single-player)
constexpr int ID_VIEW_SPLIT = 2072;    // Settings → View: 2×2 split (multiple simultaneous views)
constexpr int ID_VIEW_PIP = 2073;      // Settings → View: picture-in-picture
// 0.2.6 batch — allocated from the free 2074..2099 block (NEVER at/above 2100: that's the
// open-ended ID_THEME_SKIN_BASE range, which grows with builtinSkins()).
constexpr int ID_FMT_MP4 = 2074;         // recording format: MP4 (direct mp4 mux; see onToggleRecord)
constexpr int ID_RESUME_LAST = 2075;     // Settings → Resume last channel on launch (toggle)
constexpr int ID_FAV_EXPORT = 2076;      // Settings → Export favourites… (M3U)
constexpr int ID_FAV_IMPORT = 2077;      // Settings → Import favourites… (match by URL, then tvg-id)
constexpr int ID_LAYOUT_SAVE = 2078;     // Settings → Layout → Save layout as…
constexpr int ID_LAYOUT_APPLY_BASE = 2079;   // + saved-layout index (10 ids: 2079..2088)
constexpr int ID_LAYOUT_DELETE_BASE = 2089;  // + saved-layout index (10 ids: 2089..2098)
constexpr int kMaxSavedLayouts = 10;         // both ranges above hold exactly this many
#ifdef RABBITEARS_THEME_ENGINE
constexpr int ID_THEME_SYSTEM = 2045;     // Settings → Theme: "Follow System"
constexpr int ID_THEME_SKIN_BASE = 2100;  // + builtinSkins() index (registry-driven; above ID_DOCK_BASE)
#endif
constexpr int ID_LAYOUT_RESET = 2050;  // Settings → Layout
constexpr int ID_DOCK_BASE = 2051;     // + panel*4 + side  (12 ids: 2051..2062)
// 0.2.7 (Recording Phase 3) — from the free 2063..2069 gap (2074..2098 went to the 0.2.6 batch).
constexpr int ID_WAKE_RECORD = 2063;   // Settings → Wake this PC to record (Task Scheduler)
constexpr int ID_RULES = 2064;         // Settings → Recording Rules… (series rules manager)
constexpr int ID_WAKE_RUN_NOW = 2065;  // Settings → Run wake task now (fires --scheduled-wake)
#ifdef RABBITEARS_THEME_ENGINE
constexpr UINT_PTR kSkinAnimTimer = 0xA1;  // ~60fps repaint of the GPU transport-strip underglow
#endif

// ---- transport glyphs (Segoe MDL2) ----------------------------------------
// Segoe MDL2 Assets transport glyphs — rendered with the MDL2 glyph font on the
// square transport buttons. Play/Pause and Record/Stop-rec swap with state.
constexpr wchar_t kGlyphPlay[] = L"";
constexpr wchar_t kGlyphPause[] = L"";
constexpr wchar_t kGlyphStop[] = L"";
constexpr wchar_t kGlyphRecord[] = L"";
constexpr wchar_t kGlyphFull[] = L"";

// ---- buffer-slider bounds + local view/EPG/pane types ---------------------
// Buffer (network-caching) slider bounds in ms, snapped to kBufStepMs. This is the
// receive->show latency the user trades for smoothness on flaky streams.
constexpr int kBufMinMs = 500, kBufMaxMs = 8000, kBufStepMs = 250;

enum class ViewKind { All, Favourites, Group, Country, Playlist, Guide };
struct ViewFilter {
    ViewKind     kind = ViewKind::All;
    std::wstring group;
    long long    playlistId = 0;
    std::wstring country;  // ISO code when kind == Country
};

struct CmdBtn {
    int          id;
    const wchar_t* label;
    bool         accent;
};
constexpr CmdBtn kCmdBtns[] = {
    {ID_ADD_URL, L"+  Add Playlist", true},
    {ID_SETTINGS, L"Settings  ▾", false},
};
struct PlaylistResult {
    bool         ok = false;
    std::wstring error;
    std::wstring name;
    std::wstring source;
    bool         isUrl = true;
    M3uDocument  doc;
    int          parsed = 0;    // channels the parser produced
    int          imported = 0;  // rows inserted/updated in the DB
    int          groups = 0;    // distinct non-empty group titles in this import
};

// EPG (Refresh Guide) — one enabled playlist that carries an XMLTV URL, the fetch
// worker's per-playlist outcome, and the batch posted back to the UI thread. Fetch +
// parse run off-thread; the DB write (bulkInsertProgrammes) runs on the UI thread,
// mirroring the playlist-import split.
struct EpgTarget {
    long long    id = 0;
    std::wstring name;
    std::wstring url;
};
struct EpgFetch {
    long long              playlistId = 0;
    std::wstring           name;
    std::wstring           error;       // empty on success
    std::vector<Programme> programmes;  // parsed rows (empty on failure)
};
struct EpgResult {
    std::vector<EpgFetch> fetches;
};

// One video pane: its own surface window + libVLC player (sharing AppState::engine) + the
// channel currently in it. Split view / PIP hold several; Single holds one. Heap-allocated
// because VlcPlayer owns a worker thread (so it isn't movable), and addressed by index —
// the index lives in the video window's GWLP_USERDATA so VideoProc knows which pane it is.
struct VideoPane {
    HWND         hwnd = nullptr;   // kVideoClass window: a WS_CHILD tile, or a floating popup for PIP
    VlcPlayer    player;           // borrows AppState::engine's shared libVLC instance
    Channel      nowPlaying{};     // last channel played into this pane (for re-buffering)
    long long    nowPlayingId = 0;
    std::wstring nowPlayingName;
    bool         floating = false;  // PIP: a top-level owned popup that composites OVER the big
                                    // pane's libVLC surface (a child sibling gets occluded by it).
    // Pool of kVoutHostClass child windows (all filling the pane, all hidden except the live one)
    // that libVLC renders into — never the pane HWND directly. See VlcPlayer's vout-host pool: a
    // new stream attaches to a proven-free host so the old vout can drain without a Direct3D popout.
    // Created/sized/shown/hidden on the UI thread only; the player selects which one is live.
    std::vector<HWND> voutHosts;
};

struct AppState {
    Database   db;
    VlcEngine  engine;  // owns the shared libVLC instance; must outlive the panes (below)
    // The video panes. panes[0] is created in WM_CREATE and always exists; Split/PIP add
    // more. `active` is the focused pane — it gets channel selection, audio, the transport
    // strip, and the meters; the others play muted. ap() is the active pane.
    std::vector<std::unique_ptr<VideoPane>> panes;
    int        active = 0;
    ViewMode   viewMode = ViewMode::Single;
    VideoPane& ap() { return *panes[active]; }
    // Panes torn down by a mode switch, stopping ASYNCHRONOUSLY in the background so the UI
    // thread never blocks on a stuck stream's libVLC stop(). Reaped (join + DestroyWindow) once
    // each player's async stop finishes; force-drained at WM_DESTROY. See reapDyingPanes().
    std::vector<std::unique_ptr<VideoPane>> dyingPanes;
    HWND       hwnd = nullptr;
    HWND       nav = nullptr;
    HWND       splitter = nullptr;
    HWND       grid = nullptr;
    HWND       search = nullptr;
    HWND       btnPlay = nullptr;
    HWND       btnStop = nullptr;
    HWND       btnRec = nullptr;
    HWND       btnFull = nullptr;
    HWND       volIcon = nullptr;   // speaker glyph left of the volume slider
    HWND       volBar = nullptr;
    HWND       bufLabel = nullptr;  // "Buffer 1.5 s"
    HWND       bufBar = nullptr;    // network-caching slider (receive->show delay)
    HWND       tip = nullptr;       // shared tooltip (volume slider, buffer slider, meter)
    HWND       status = nullptr;
    HWND       bufferMeter = nullptr;
#ifdef RABBITEARS_THEME_ENGINE
    bool       skinStripOn = false;  // GPU transport-strip underglow available (theme-engine spike)
#endif
    HWND       meterSpectrum = nullptr;  // modular LED mini-meters (Settings → Meters)
    HWND       meterSignal = nullptr;
    HWND       meterBitrate = nullptr;
    HWND       meterFrames = nullptr;
    HFONT      uiFont = nullptr;
    HFONT      titleFont = nullptr;
    HFONT      glyphFont = nullptr;  // Segoe MDL2 Assets, for the speaker glyph
    UINT       dpi = 96;
    int        sidebarW = 240;  // nav width in px (draggable via the splitter)
    int        cmdHover = -1;  // hovered toolbar button index
    int        capHover = -1;  // hovered caption button (0 min,1 max,2 close)
    bool       fullscreen = false;
    bool       videoOnly = false;     // hide all chrome in-window (Settings→Video only; dbl-click/Esc exits)
    bool       videoDragging = false; // dragging the window by the video (video-only has no title bar)
    POINT      videoDragStart{};      // cursor (screen) at drag start
    POINT      videoDragOrigin{};     // window top-left at drag start
    bool       videoDragMoved = false;  // crossed the drag threshold? (ignore sub-threshold jitter)
    bool       draggingPip = false;    // the drag above is moving the floating PIP, not the main window
    bool       pipMoved = false;       // user dragged the PIP — honour pipPos instead of the default corner
    POINT      pipPos{};               // PIP top-left in main-client coords (used when pipMoved)
    bool       resizingPip = false;    // dragging the PIP's bottom-right resize corner (not moving it)
    POINT      pipResizeStart{};       // cursor (screen) at resize start
    SIZE       pipResizeOrigin{};      // PIP size at resize start
    int        pipW = 0, pipH = 0;     // user-chosen PIP size in px (0 = the default dp(220)×dp(124));
                                       // persisted as "pip_size", fed into layout()'s VideoGridOpts
    bool       resumeLast = true;      // auto-play the last channel on launch (setting "resume_last")
    WINDOWPLACEMENT prevPlacement{};  // saved to restore from fullscreen
    LONG       prevStyle = 0;         // window style saved on entering fullscreen
    bool       busy = false;
    HWND       loadingDlg = nullptr;    // modeless "please wait" box during an EPG fetch (null = idle)
    long long  activeScheduleId = 0;   // id of the schedule currently owning a recorder (0 = none)
    int        schedulePane = -1;      // pane index the active schedule records on (recording is
                                       // per-pane; the user may switch panes mid-record) — -1 = none
    bool       schedulerReconciled = false;  // one-time startup reset of stale "Recording" rows
    bool       wakeToRecord = true;   // register a Windows task to wake this PC for a recording
                                      // (setting "wake_to_record"; see platform/WakeScheduler)
    long long  wakeTaskFor = -1;      // the schedule start the wake task currently targets
                                      // (0 = no task, -1 = never synced). Keyed on the UNCLAMPED
                                      // start so the ~30s tick doesn't re-register COM every pass.
    long long  rulesExpandedAt = 0;   // last EPG->schedule rule expansion (unix s); throttles the tick
    std::wstring recFormat = L"ts";  // recording container: "ts" | "mkv" | "mp4"
    bool       hideDead = false;     // hide unavailable (dead/geo-blocked) channels
    bool       categoryActive = false;    // is the Categories include-filter on?
    std::set<std::wstring> categories;    // included group titles when active
    bool       showSpectrum = true;       // Settings → Meters visibility (persisted)
    bool       showSignal = true;
    bool       showBitrate = false;
    bool       showFrames = false;
    ViewFilter filter;
    std::vector<ViewFilter> navFilters;  // indexed by tree item lParam
    SpectrumTap spectrumTap;             // read-only WASAPI process-loopback → spectrum meter
    DockLayout  dock;                    // user-arrangeable region layout (persisted)
    std::vector<DockLayout::Gutter> gutters;  // splitter gutters from the last layout()
    bool        draggingGutter = false;
    DockLayout::Gutter dragGutter{};
    ULONGLONG   gutterFlushTick = 0;  // last paced sync-repaint flush during a gutter drag
    // Drag-to-redock: a small grip per region, a translucent drop-zone overlay, and
    // the in-flight drag target.
    HWND        gripNav = nullptr, gripVideo = nullptr, gripGrid = nullptr;
    HWND        dockOverlay = nullptr;      // layered highlight, created lazily
    RECT        panelRects[kPanelCount]{};  // last-laid-out rects, for drop hit-testing
    RECT        paneBounds[4]{};             // last-laid-out video-pane rects (active-border paint)
    bool        panelDragActive = false;
    Panel       panelDragFrom = Panel::Nav;
    Panel       panelDropTo = Panel::Nav;
    DockSide    panelDropSide = DockSide::Left;
    bool        panelDropValid = false;
};

struct BtnRect {
    RECT rc;
    int  id;
};

// ---- cross-file function prototypes (defined across MainWindow*.cpp) ------
int dp(int v, UINT dpi);
AppState* stateOf(HWND h);
void setStatus(AppState* st, const std::wstring& s);
int cmdBarH(UINT dpi);
int navWidth(UINT dpi);
int stripH(UINT dpi);
int capW(UINT dpi);
int measureText(HWND hwnd, HFONT font, const std::wstring& s);
void applyDarkChrome(HWND hwnd);
RECT captionRect(HWND hwnd, AppState* st, int i);
std::vector<BtnRect> cmdButtonRects(HWND hwnd, AppState* st);
void drawCaptionGlyph(HDC dc, const RECT& r, int which, COLORREF color);
void drawCmdBar(HWND hwnd, AppState* st, HDC target);
void positionFloatingPip(AppState* st);
void layout(HWND hwnd, AppState* st);
const DockLayout::Gutter* gutterAt(AppState* st, POINT pt);
void paintGutters(AppState* st, HDC hdc);
void persistDock(AppState* st);
void applyDockChange(HWND hwnd, AppState* st);
void dockToEdge(AppState* st, Panel p, DockSide side);
LRESULT CALLBACK DropOverlayProc(HWND h, UINT m, WPARAM w, LPARAM l);
HWND ensureDropOverlay(HWND parent, AppState* st);
void beginPanelDrag(HWND parent, AppState* st, Panel p);
void updateDockTarget(HWND hwnd, AppState* st, POINT pt);
void endPanelDrag(HWND hwnd, AppState* st, bool commit);
LRESULT CALLBACK DockGripProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK VSplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void updateCounts(AppState* st);
void applyChannelFilters(AppState* st, std::vector<Channel>& ch);
void loadForFilter(AppState* st);
HTREEITEM navInsert(HWND nav, HTREEITEM parent, const std::wstring& text, LPARAM param, bool bold);
std::wstring countryLabel(const std::wstring& code);
void refreshNav(AppState* st);
void playChannelInPane(AppState* st, const Channel& c, int idx);
void playChannel(AppState* st, const Channel& c);
std::wstring bufLabelText(int ms);
void setBufferMs(AppState* st, int ms, bool replay);
std::wstring nameFromSource(const std::wstring& src, bool isUrl);
void startPlaylistWorker(AppState* st, const std::wstring& source, bool isUrl, const std::wstring& name);
void onAddUrl(AppState* st);
void onOpenFile(AppState* st);
void onPlaylistDone(AppState* st, PlaylistResult* res);
void onEpgRefresh(AppState* st);
void onEpgDone(AppState* st, EpgResult* res);
void onEpgGuide(AppState* st);
void promptSetGuideUrl(HWND hwnd, AppState* st, long long pid);
void toggleFullscreen(AppState* st);
void toggleVideoOnly(AppState* st);
VideoPane* addPane(HWND hwnd, AppState* st, int index, bool floating = false);
HWND makeVoutHost(AppState* st, int paneIdx);  // UI thread: create + register one pane vout host
void setActivePane(AppState* st, int idx);
void applyViewMode(AppState* st, ViewMode mode);
void reapDyingPanes(AppState* st, bool force);  // reap async-torn-down panes (force at exit)
std::wstring recordingsDir();
std::wstring recordingPath(const std::wstring& channelName, const std::wstring& ext);
void onToggleRecord(AppState* st);
void onSchedulerTick(AppState* st);
void scheduleFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName, const std::wstring& title, long long startUtc, long long stopUtc);
void onManageSchedules(AppState* st);
// Recording Phase 3 (0.2.7)
void syncKeepAwake(AppState* st);           // suppress sleep while any pane records
void syncWakeFromSchedules(AppState* st);   // (re)register / clear the Windows wake task
// Materialize rule matches into schedules; returns #added. Throttled to kRuleExpandIntervalSeconds
// unless `force` (a guide refresh / a new or re-enabled rule) — the EPG query is heavy.
int  expandRecordingRules(AppState* st, bool force = false);
void recordSeriesFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName, const std::wstring& title);
void onManageRules(AppState* st);
std::wstring joinCategories(const std::set<std::wstring>& s);
std::set<std::wstring> splitCategories(const std::wstring& s);
void onCategories(AppState* st);
void syncSpectrumTap(AppState* st);
void resetStatMeters(AppState* st);
void onMeters(AppState* st);
void showSettingsMenu(HWND hwnd, AppState* st, const RECT& anchor);
#ifdef RABBITEARS_THEME_ENGINE
void remakeUiFonts(AppState* st);
void applyActiveSkin(HWND hwnd, AppState* st, bool repaint);
void setSkinSelection(HWND hwnd, AppState* st, const char* sel);
#endif

}  // namespace mw
}  // namespace rabbitears
