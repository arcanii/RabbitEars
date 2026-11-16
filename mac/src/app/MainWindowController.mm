// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#import <IOKit/pwr_mgt/IOPMLib.h>  // IOPMAssertion — keep-awake while recording

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <filesystem>  // -openLogFolder: derives the log path from the DB path
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/GuideSources.h"  // a guide link naming several guides: split + merge
#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/M3uWriter.h"
#include "core/RecordingRules.h"
#include "core/RecordingScheduler.h"
#include "core/UrlCanon.h"
#include "core/XmltvParser.h"
#include "core/XtreamClient.h"  // catch-up: timeshift URLs
#include "models/Programme.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
#include "ui/VideoGrid.h"  // computeVideoPanes + ViewMode — multi-view pane geometry
#include "platform/Log.h"
#include "platform/HttpProgress.h"  // the guide download's running byte count
#include "platform/LogSecrets.h"
#include "platform/TimeZone.h"  // catch-up: the server's zone on the programme's day
#include "platform/Updater.h"

#if __has_include("generated/version.h")
#include "generated/version.h"
#endif
#ifndef RE_VERSION_FULL_W
#define RE_VERSION_FULL_W L"dev"  // fallback when the generated header is absent
#endif
#ifndef RE_VERSION_W
#define RE_VERSION_W L"dev"       // marketing version shown in the Terms dialog
#endif

#import "AppDelegate.h"
#import "MeterView.h"
#import "MetersDialog.h"
#import "PlaylistsDialog.h"
#import "CategoriesDialog.h"
#import "LogoLoader.h"
#import "DeadLinkSweep.h"
#import "VodSync.h"
#import "RecordingsWindowController.h"
#include "ScheduleStatusWriter.h"
#import "MeterBridgeWindowController.h"
#import "TankView.h"
#include "MeterLayout.h"  // the meters' size (stage A): heights, widths, labels
#import "SpectrumTap.h"
#import "TermsDialog.h"
#import "TvGuideWindowController.h"
#include "GuideRefresh.h"  // Refresh Guide's worker
#import "VlcEngineMac.h"
#import "VlcPlayerMac.h"
#import "Tr.h"

using namespace rabbitears;
using namespace rabbitears::mac;  // MeterKind / MeterStyle / MeterConfig + the meter codecs
using namespace rabbitears::i18n;  // StringId / Tr / TrF

// NSTableView that plays the selected row on Return/Enter (the keyboard peer of the
// double-click action), so the grid is fully keyboard-navigable.
@interface RETableView : NSTableView
@end
@implementation RETableView
- (void)keyDown:(NSEvent*)e {
    const unichar ch = e.charactersIgnoringModifiers.length
        ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    if ((ch == NSCarriageReturnCharacter || ch == NSEnterCharacter)
        && self.selectedRow >= 0 && self.doubleAction) {
        [NSApp sendAction:self.doubleAction to:self.target from:self];
        return;
    }
    [super keyDown:e];
}
@end

// A container that floats the meters over the video; the user can drag it anywhere.
// Reports its new position (via onMoved) after each drag so the controller persists it.
@interface DraggableMeterBar : NSView
@property (nonatomic, copy) void (^onMoved)(void);
// The top edge (kResizeBand pt) resizes the meters instead of moving the bar — the mac reading of
// Win32's strip-edge drag: `onResize` gets the bar's wanted height (its bottom stays put), and
// `onMoved` persists the result on mouse-up.
@property (nonatomic, copy) void (^onResize)(CGFloat barHeight);
@end

@implementation DraggableMeterBar {
    NSPoint _grab;      // cursor offset within the bar at mouse-down
    BOOL    _resizing;  // the drag began on the top edge
}
static const CGFloat kResizeBand = 5;
- (void)resetCursorRects {
    if (!self.onResize) return;
    const NSRect b = self.bounds;
    [self addCursorRect:NSMakeRect(0, b.size.height - kResizeBand, b.size.width, kResizeBand)
                 cursor:NSCursor.resizeUpDownCursor];
}
- (void)mouseDown:(NSEvent*)e {
    _grab = [self convertPoint:e.locationInWindow fromView:nil];
    _resizing = self.onResize && _grab.y >= self.bounds.size.height - kResizeBand;
}
- (void)mouseDragged:(NSEvent*)e {
    NSView* pane = self.superview;
    if (!pane) return;
    const NSPoint p = [pane convertPoint:e.locationInWindow fromView:nil];
    if (_resizing) {
        const CGFloat top = std::min(p.y, pane.bounds.size.height);  // the bar's bottom stays put
        self.onResize(std::max<CGFloat>(1, top - self.frame.origin.y));
        return;
    }
    const CGFloat maxX = std::max(0.0, pane.bounds.size.width - self.frame.size.width);
    const CGFloat maxY = std::max(0.0, pane.bounds.size.height - self.frame.size.height);
    NSRect f = self.frame;
    f.origin.x = std::clamp(p.x - _grab.x, 0.0, maxX);
    f.origin.y = std::clamp(p.y - _grab.y, 0.0, maxY);
    self.frame = f;
}
- (void)mouseUp:(NSEvent*)__unused e {
    _resizing = NO;
    [self.window invalidateCursorRectsForView:self];
    if (_onMoved) _onMoved();
}
@end

// One video pane in the multi-view area: a video surface (a child of the video container)
// + its libVLC player + what it's currently playing. Single view uses one pane; Split/2×2
// and PiP use several. The pane OWNS its player; its view is owned by the container (super).
struct MacVideoPane {
    NSView*                                view = nil;
    std::unique_ptr<rabbitears::VlcPlayerMac> player;
    rabbitears::Channel                    channel;      // currently playing (empty when idle)
    long long                              channelId = 0;
    bool                                   everPlayed = false;  // reached Playing since last play() (dead-status heuristic)
    // Catch-up: this pane plays an archive (-playCatchup:), and whether it ever reached Playing — an
    // archive that has nothing for that time fails before it plays, and the status says so plainly.
    bool                                   catchup = false;
    bool                                   catchupPlayed = false;
};

// Filter popup tags.
// Nav filter tags. kFilterMovies is a SECTION HEAD, not a view: selecting it loads nothing and
// only prompts for a category, because "all movies" is 40k+ rows on a real provider and a nav
// click must never be that. kFilterMovieGroup is one VOD category, and is kind-scoped in SQL
// (moviesByGroup) so a VOD category cannot collide with a live group of the same name.
enum {
    kFilterAll = 0, kFilterFavourites = 1, kFilterGroup = 2, kFilterCountry = 3,
    kFilterMovies = 4, kFilterMovieGroup = 5
};

// Most rows the grid will materialise in one fill (Win32 kMaxGridRows parity). The real cost is
// not the NSTableView — it is building `_channels`, which on a big library was measured on Windows
// at 1485 ms for All Channels and 1626 ms PER KEYSTROKE for search. The queries ask for
// kMaxGridRows + 1 on purpose: getting that many back is what proves the library holds MORE,
// exactly, instead of guessing from `size() == limit`, which cannot tell a truncated view from one
// that happens to hold exactly the cap.
constexpr int kMaxGridRows = 5000;

// Chrome metrics, shared by showWindow and the hide/show-chrome relayout.
static const CGFloat kBarH = 46;     // top command bar height
static const CGFloat kStatusH = 22;  // bottom status/volume bar height
// Left edge of the bottom bar's right-hand cluster (meter toggle | mute | volume), measured from
// the right edge of the content view. ONE constant on purpose: _meterBtn is placed at
// `w - kRightClusterW` and -layoutBottomBar ends the status line and the transport cluster there,
// so the two can never disagree — they did, and the transport was drawn 8 pt over the button.
static const CGFloat kRightClusterW = 162;
static const CGFloat kMeterH = 24;   // meter line height
static const CGFloat kMeterGap = 8;  // gap between adjacent meters
// Per-kind meter widths (index = (int)MeterKind: Spectrum, Signal, Bitrate, Frames).
static const CGFloat kMeterW[4] = {180, 64, 130, 96};

// PiP inset sizing. Default 16:9 at 240×135; the user can drag-resize between the min and
// 60% of the container (enforced in -applyVideoPaneLayout and -paneDragged:).
static const int kPipDefaultW = 240;
static const int kPipDefaultH = 135;
static const int kPipMinW = 160;
static const int kPipMinH = 90;

// Named saved layouts (Settings ▸ Layouts). Stored in the settings K/V — "layout_names" is a
// newline-joined index (menu order); each layout lives at "layout_saved_<name>". Same scheme
// as Win32, but a mac-local serialization: mac's "layout" is the multi-view arrangement (view
// mode + per-pane channel ids + PiP geometry), not Win32's dock-panel layout.
static const int kMaxSavedLayouts = 10;
static const int kMaxPanes = 4;  // 2×2 is the largest grid

@interface MainWindowController ()
// Post-Terms-of-Use startup (playback attach, meter tap, stats timer, resume auto-play,
// recording scheduler) — deferred until the window is shown and the ToU sheet (if any) is
// accepted, so none of it runs behind a hidden gate on a non-activating launch.
- (void)finishStartup;
@end

// Set at quit: a running Refresh Guide stops at its next download or transaction boundary.
namespace {
std::atomic<bool> g_epgCancel{false};
// The volume slider (0..1), for the Spectrum tap's RT thread: the tap hears the output AFTER libVLC's
// gain, and the audio needle reads the PROGRAMME's level whatever the volume (Win32 a592ee9).
std::atomic<float> g_tapVolume{1.0f};
// What the volume took off, given back (Win32 SpectrumTap::programmeDbfs). libVLC's mac output (auhal)
// applies the slider v as a HAL-unit gain of v^3 — the tap hears that — so it is undone by the same law.
// A slider at 0 (muted) leaves nothing to raise: the floor, as silence is; never past full scale.
float programmeDbfs(float capturedDb, float slider) {
    const float gain = slider * slider * slider;
    if (capturedDb <= -120.0f || !(gain > 1e-5f)) return -120.0f;
    if (gain >= 1.0f) return capturedDb;
    return std::min(capturedDb - 20.0f * std::log10(gain), 3.0103f);
}
}  // namespace

@implementation MainWindowController {
    NSWindow*      _window;
    NSSearchField* _search;
    NSPopUpButton* _filter;
    std::set<std::wstring> _categoryFilter;  // include-set of group-titles ("category_filter"); empty = show all
    RETableView*   _table;
    LogoLoader*    _logoLoader;         // async channel-logo image cache (app-lifetime)
    NSImage*       _logoPlaceholder;    // shown while a logo loads / for logo-less channels
    BOOL           _logoReloadScheduled;  // coalesces logo-load reloads to one per runloop turn
    NSView*        _videoView;
    NSMenu*        _videoMenu;    // right-click context menu on any pane's video (Win32-parity view menu)
    MeterView*     _meters[4];   // Spectrum / Signal / Bitrate / Frames (index = (int)MeterKind)
    DraggableMeterBar* _meterBar;  // floats the meters over the video; user-draggable
    NSTextField*   _emptyHint;   // "no channels yet" hint centered over the empty video pane
    NSTextField*   _status;
    NSSlider*      _volume;      // bottom-bar volume (0..100)
    NSButton*      _muteBtn;     // 🔊 / 🔇 toggle
    NSButton*      _meterBtn;    // bottom-bar show/hide-meters toggle
    NSButton*      _recBtn;      // top-bar record toggle (● / ■), tracks the active pane
    // Top-bar buttons that are otherwise setup-locals — held (UNRETAINED, like _recBtn/_muteBtn:
    // _topBar owns them for app lifetime) so a live language switch can relabel them in place.
    NSButton*      _addBtn;      // "+ Add Playlist"
    NSButton*      _setBtn;      // ⚙ gear
    NSButton*      _stopBtn;     // "Stop"
    // ---- transport cluster (bottom bar). All UNRETAINED, matching _addBtn/_setBtn/_stopBtn:
    // they are owned by the content view's subview array for the app's lifetime.
    NSButton*      _pauseBtn;    // play/pause toggle
    NSButton*      _skipBackBtn; // -10s
    NSButton*      _skipFwdBtn;  // +10s
    NSSlider*      _seek;        // the scrub bar
    NSTextField*   _seekTime;    // "12:34 / 1:45:00"
    BOOL           _seekShown;   // is the cluster currently laid out + visible?
    long long      _panesGeneration;  // ++ on every pane-set change; scopes the post-seek latch
    int            _seekHideTicks;   // consecutive !want ticks (hysteresis — see -updateTransport)
    BOOL           _seekDragging;    // thumb held down; suppress sample-driven updates
    long long      _seekLatchUntilMs;  // steady-clock ms: ignore samples until then (post-seek)
    long long      _seekLatchTarget;   // the position we asked for, shown during the latch
    long long      _seekLatchPaneId;   // pane generation the latch belongs to (0 == none)
    std::wstring   _recFormat;   // recording container: "ts" (default) / "mp4"
    BOOL           _resumeLast;  // auto-play the last channel on launch (setting "resume_last", default on)
    BOOL           _hideDead;    // Settings ⚙ ▸ Channels ▸ Hide unavailable channels ("hide_dead")
    BOOL           _gridTruncated;  // last grid fill hit kMaxGridRows and the library holds more
    BOOL           _vodSyncRunning; // mirrors rabbitears::mac::vodSyncRunning() for menu labelling
    BOOL           _sweepRunning;   // mirrors deadLinkSweepRunning() for menu labelling
    rabbitears::mac::VodSyncReport _vodPendingReport;  // handed from the completion to the deferred apply
    rabbitears::mac::DeadLinkReport _deadLinkPendingReport;  // same, for the dead-link sweep
    BOOL           _startupFinished;  // YES once -finishStartup ran (ToU accepted); gates the menu until then
    unsigned int   _keepAwake;   // IOPMAssertion id held while any recording runs (0 == none)
    IOPMAssertionID _keepDisplayAwake;  // display-sleep/screen-saver assertion held while fullscreen or video-only
    // Recording scheduler (Phase 5/6): a DEDICATED headless recorder drives scheduled + rule
    // recordings, independent of the visible panes' manual recorders. A ~30s tick applies the
    // shared planScheduler() core and expands EPG rules.
    std::unique_ptr<VlcPlayerMac> _scheduleRecorder;
    NSTimer*       _schedulerTimer;
    // Search debounce. Unlike the app-lifetime timers here, this one is invalidated and replaced
    // on every keystroke, so it is RETAINED explicitly: after -invalidate the run loop drops its
    // own reference immediately, and a bare ivar would then dangle until the next assignment —
    // the exact MRC shape that has produced three shipped over-release crashes in this file.
    NSTimer*       _searchTimer;
    long long      _activeScheduleId;   // the schedule currently on _scheduleRecorder (0 == none)
    rabbitears::ScheduledRecording _activeScheduleRow;  // its row as listed when it started (identity)
    BOOL           _schedulerReconciled; // one-time capture of stale "Recording" rows (below)
    // Stale Recording rows from a previous session, each reset to Pending only once its reset LANDS
    // — retried every tick (a reset lost to contention used to latch, blocking every schedule until
    // that row's stop time, which then closed it out as a false Done).
    std::vector<long long> _staleRecordingIds;
    // Every terminal schedule status goes through this: a write lost to DB contention is
    // remembered and replayed (ScheduleStatusWriter.h). Shared with the Recordings window.
    std::unique_ptr<rabbitears::mac::ScheduleStatusWriter> _statusWriter;
    // Catch-up: channel id -> days of archive (Database::channelArchiveDays) — the grid's ↺ markers
    // and what -playCatchup: checks. Re-read whenever the provider sync writes the flags.
    std::unordered_map<long long, int> _archiveDays;
    long long      _rulesExpandedAt;    // last rule-expansion time (throttled; 0 == never)
    id             _recordingsWC;       // RecordingsWindowController (lazily created; app-lifetime)
    NSView*        _topBar;      // top command bar (add / settings / search / filter)
    NSSplitView*   _split;       // channel grid | video
    BOOL           _gridHidden;    // View ▸ Hide Channel List
    BOOL           _toolbarHidden; // View ▸ Hide Toolbar
    BOOL           _videoOnly;     // View ▸ Video Only — all chrome hidden, video fills
    BOOL           _metersHidden;  // bottom-bar toggle: hide the whole meter line
    CGFloat        _meterPosX, _meterPosY;  // meter-bar position as a 0..1 fraction of the pane
    int            _meterHeightDp;   // `meter_height` (dp, Win32's key): 30 standard, 50 Large, 72 Extra large
    BOOL           _meterLabels;     // `meter_labels`: each meter's short name under it (taller rows, the bridge)
    NSTextField*   _meterLabelViews[4];
    MeterBridgeWindowController* _meterBridge;  // the meter bridge window (lazily made; app-lifetime)
    // The data-flow tank (Win32's buffering visualizer): the bar's right-most meter while `buffer_hidden`
    // is "0" (mac: off unless turned on — Settings ▸ Meters ▸ Data flow; right-click it to hide).
    TankView*      _tank;
    NSTextField*   _tankLabel;
    BOOL           _dataFlowOn;
    BOOL           _pipMoved;               // the user moved/resized the PiP inset (else default corner)
    CGFloat        _pipPosX, _pipPosY;      // PiP inset position, as a 0..1 fraction of the free travel
    CGFloat        _pipW, _pipH;            // PiP inset size in px (persisted); 0 == use the default
    BOOL           _pipResizing;            // latched at drag-start: this drag resizes (else moves)
    NSSize         _pipResizeOriginSize;    // inset size when this resize drag began (aspect snap)
    NSPoint        _pipResizeAccum;         // cumulative RAW drag translation since that moment
    id             _escMonitor;    // local key monitor for Esc while video-only (nil otherwise)

    std::unique_ptr<Database>     _db;
    std::unique_ptr<VlcEngineMac> _engine;   // shared libVLC instance; every player borrows it
    std::vector<std::unique_ptr<MacVideoPane>> _panes;  // 1 pane (Single) or N (Split/2×2/PiP)
    int                           _activePane;      // index into _panes: drives audio/meters/volume
    ViewMode                      _viewMode;        // Single / Split / Pip
    NSView*                       _videoContainer;  // the split's right pane; holds the pane views + meter bar
    VlcPlayerMac*                 _player;          // alias → the active pane's player (panes own them)
    NSTimer*                      _statsTimer;   // 250ms libVLC-stats poll → _statMeter
    double                        _bitrateMax;   // rolling throughput peak for the meter scale
    SpectrumTap*                  _spectrumTap;      // Core Audio process tap → the Spectrum meter
    rabbitears::mac::MeterConfig  _meterCfg[4];      // per-kind config (from the dialog + persistence)
    int                           _spectrumSilentTicks;   // audible-but-tap-silent polls (denial detection)
    BOOL                          _spectrumEverHadEnergy;  // tap has produced audio => capture granted this session
    std::vector<Channel>          _channels;
    long long                     _currentPid;             // loaded playlist (0 = none/all)
    uint64_t                      _loadToken;              // only the newest URL load's result applies
    BOOL                          _epgRefreshing;          // a Refresh Guide is running (one at a time)
    id                            _guideWC;                // TvGuideWindowController (lazily created; app-lifetime)
    CGFloat                       _gridWidth;              // channel-grid width; the video pane fills the rest
    int                           _preMuteVolume;          // volume to restore when un-muting
}

- (instancetype)init {
    if ((self = [super init])) {
        _db = std::make_unique<Database>();
        _engine = std::make_unique<VlcEngineMac>();
        _engine->init();  // create the shared libVLC instance once (loads the plugins)
        _viewMode = ViewMode::Single;
        _activePane = 0;
        // Pane 0 (the active pane). Its player is created now so early -showWindow calls
        // (e.g. setVolume) work; its video surface is built in -showWindow with the window.
        auto pane = std::make_unique<MacVideoPane>();
        pane->player = std::make_unique<VlcPlayerMac>();
        pane->player->init(*_engine);
        _panes.push_back(std::move(pane));
        _player = _panes[0]->player.get();  // alias to the active pane's player
    }
    return self;
}

static NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:utf8FromWide(w).c_str()] ?: @"";
}
static std::wstring ws(NSString* s) { return wideFromUtf8(s.UTF8String ?: ""); }

// A readable display name from a playlist source: the file / last-path-segment stem
// (minus any .m3u/.m3u8), else the URL host, else the raw source. The full URL/path is
// still stored separately as the playlist's `source`; users can rename afterwards.
static std::wstring friendlyName(const std::wstring& src, bool isUrl) {
    std::wstring s = src;
    if (isUrl) {
        const size_t q = s.find_first_of(L"?#");  // drop query/fragment
        if (q != std::wstring::npos) s.erase(q);
    }
    const size_t slash = s.find_last_of(L"/\\");
    std::wstring last = (slash == std::wstring::npos) ? s : s.substr(slash + 1);
    auto endsWithCI = [](const std::wstring& a, const std::wstring& suf) {
        if (a.size() < suf.size()) return false;
        for (size_t i = 0; i < suf.size(); ++i) {
            wchar_t c = a[a.size() - suf.size() + i];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
            if (c != suf[i]) return false;
        }
        return true;
    };
    if (endsWithCI(last, L".m3u8")) last.erase(last.size() - 5);
    else if (endsWithCI(last, L".m3u")) last.erase(last.size() - 4);
    if (!last.empty()) return last;
    if (isUrl) {  // no file segment (e.g. http://host/) — fall back to the host
        std::wstring h = src;
        const size_t scheme = h.find(L"://");
        if (scheme != std::wstring::npos) h.erase(0, scheme + 3);
        const size_t slash2 = h.find_first_of(L"/?#");
        if (slash2 != std::wstring::npos) h.erase(slash2);
        if (!h.empty()) return h;
    }
    return src;
}

- (void)showWindow {
    const NSRect frame = NSMakeRect(0, 0, 980, 640);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = Tr(StringId::AppName);
    [_window center];
    _window.releasedWhenClosed = NO;  // we own it via the ivar
    _window.contentMinSize = NSMakeSize(560, 360);  // keep both split panes usable
    _window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;  // ⌃⌘F / green button
    _window.frameAutosaveName = @"RabbitEarsMainWindow";  // remember size + position across launches
    _window.delegate = self;  // windowDidEnter/ExitFullScreen: -> screen-saver (display-sleep) assertion
    NSView* content = _window.contentView;
    const NSSize cs = content.bounds.size;  // real content size — frameAutosave may restore a larger frame

    // NB: the window is deliberately NOT shown here. Opening the DB runs migrate(), and schema v9
    // rewrites every stored stream_url + merges colliding rows — seconds of main-thread work on a
    // very large library. Showing an EMPTY window in front of that is worse than showing none: the
    // UI below is not built yet, so the user gets a blank frame that macOS then ghosts as "not
    // responding", and every ordinary (fast) launch pays a visible empty-window flash for it.
    // The window is shown once it has real content, at the end of -showWindow. If the migration
    // pause ever needs covering, the fix is a progress sheet gated on a pending migration, not an
    // unconditional early paint.
    std::wstring err;
    if (!_db->open(Database::defaultDbPath(), &err)) {
        diag::error(L"DB open failed: " + err);
        _db.reset();  // make the unusable state explicit; the guards below surface it
    }
    if (_db) _statusWriter = std::make_unique<rabbitears::mac::ScheduleStatusWriter>(_db.get());
    if (_db) _archiveDays = _db->channelArchiveDays();  // the grid's ↺ markers, before the first fill

    // Log level FIRST, ahead of every other restore: a tester who set Detailed/Everything to chase
    // a startup problem must have it in force before the interesting logging happens. It is a
    // plain atomic, so this is just seeding state. Win32 restores it at the same point, and the
    // stored form is the shared levelToString/levelFromString spelling so a log_level written by
    // either platform is understood by the other.
    if (_db) {
        if (auto lv = _db->getSetting(L"log_level"); lv && !lv->empty())
            diag::setLevel(diag::levelFromString(utf8FromWide(*lv), diag::Level::Info));
        // Logged AFTER the level is applied, so it is itself subject to it: a tester reading the
        // log can tell at a glance what threshold produced the file, and its absence is the honest
        // signal that the level is below Info.
        diag::info(L"log level: " + wideFromUtf8(diag::levelToString(diag::level())));
        // The provider logins to mask in the log (platform/LogSecrets.h), registered before
        // anything below can log a playlist, guide or stream URL — the Win32 peer does the same
        // at the same point (MainWindow.cpp, the first DB block).
        for (const auto& pl : _db->listPlaylists()) {
            diag::addSecretsFromUrl(pl.sourceUrl);
            diag::addSecretsFromUrl(pl.epgUrl);
        }
        diag::scrubPreviousLog();  // one written before masking existed kept them in clear
    }

    // The Terms-of-Use gate used to run here (blocking, before the window was built). It now
    // runs at the END of -showWindow, as a SHEET on the shown window — see below.
    [self loadMeterConfig];  // per-kind enable/style/colours from settings

    // ---- top bar (one row): [+ Add Playlist] [⚙] … [search] [filter] [Stop]
    //      — the mac peer of the Win32 command bar (kCmdBtns = + Add Playlist, Settings).
    const CGFloat barH = kBarH;
    NSView* bar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, cs.height - barH, cs.width, barH)];
    _topBar = bar;
    bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    // Left: the accent "+ Add Playlist" (prompts for a URL) and the Settings menu.
    NSButton* addBtn = [NSButton buttonWithTitle:Tr(StringId::CmdAddPlaylist)
                                          target:self action:@selector(addPlaylist:)];
    addBtn.frame = NSMakeRect(12, 9, 138, 28);
    addBtn.bezelColor = NSColor.controlAccentColor;  // accent, like the Win32 button
    [bar addSubview:addBtn];
    _addBtn = addBtn;  // hold for live relabel (unretained; the bar retains it)

    // A gear that pops the Open/Manage/Meters/Updates/About menu (showSettings:).
    NSImage* gear = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:Tr(StringId::TooltipSettings)];
    NSButton* setBtn = [NSButton buttonWithImage:gear target:self action:@selector(showSettings:)];
    setBtn.frame = NSMakeRect(158, 9, 40, 28);
    setBtn.toolTip = Tr(StringId::TooltipSettings);
    [bar addSubview:setBtn];
    _setBtn = setBtn;  // hold for live relabel

    // Record toggle for the active pane (● idle / ■ recording). Between the gear and search.
    NSImage* recImg = [NSImage imageWithSystemSymbolName:@"record.circle" accessibilityDescription:Tr(StringId::TooltipBtnRecord)];
    _recBtn = [NSButton buttonWithImage:recImg target:self action:@selector(toggleRecord:)];
    _recBtn.frame = NSMakeRect(206, 9, 40, 28);
    _recBtn.toolTip = Tr(StringId::MacMainWindowRecordActivePaneTooltip);
    [bar addSubview:_recBtn];

    // Right (pinned): Stop, the filter popup, and the stretchy search field between.
    NSButton* stopBtn = [NSButton buttonWithTitle:Tr(StringId::MacMainWindowStopButton) target:self action:@selector(stop:)];
    stopBtn.frame = NSMakeRect(cs.width - 92, 9, 80, 28);
    stopBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:stopBtn];
    _stopBtn = stopBtn;  // hold for live relabel

    _filter = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(cs.width - 270, 9, 170, 28) pullsDown:NO];
    _filter.target = self;
    _filter.action = @selector(filterChanged:);
    _filter.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:_filter];

    _search = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(284, 10, cs.width - 562, 26)];
    _search.placeholderString = Tr(StringId::SearchChannelsPlaceholder);
    _search.autoresizingMask = NSViewWidthSizable;
    _search.delegate = self;  // controlTextDidChange: -> live filter
    [bar addSubview:_search];

    [content addSubview:bar];

    // ---- bottom bar: status line (left) + volume (right) ----
    const CGFloat statusH = kStatusH;
    _status = [NSTextField labelWithString:Tr(StringId::MacMainWindowStatusReady)];
    _status.frame = NSMakeRect(12, 3, cs.width - kRightClusterW - 12, statusH - 5);  // same edge
    _status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    _status.textColor = NSColor.secondaryLabelColor;
    [content addSubview:_status];

    int vol0 = 100;
    if (_db) {
        const auto v = _db->getSetting(L"volume");
        if (v && !v->empty()) vol0 = (int)std::wcstol(v->c_str(), nullptr, 10);
    }
    _preMuteVolume = vol0 > 0 ? vol0 : 100;
    _muteBtn = [NSButton buttonWithTitle:(vol0 == 0 ? @"🔇" : @"🔊")
                                  target:self action:@selector(toggleMute:)];
    _muteBtn.bordered = NO;
    _muteBtn.frame = NSMakeRect(cs.width - 132, 1, 24, 20);
    _muteBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:_muteBtn];

    _meterBtn = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"waveform"
                                                  accessibilityDescription:Tr(StringId::MacMainWindowToggleMetersTooltip)]
                                   target:self action:@selector(toggleMeters:)];
    _meterBtn.bordered = NO;
    _meterBtn.toolTip = Tr(StringId::MacMainWindowToggleMetersTooltip);
    _meterBtn.frame = NSMakeRect(cs.width - kRightClusterW, 1, 24, 20);
    _meterBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _meterBtn.alphaValue = _metersHidden ? 0.35 : 1.0;
    [content addSubview:_meterBtn];

    _volume = [NSSlider sliderWithValue:vol0 minValue:0 maxValue:100
                                 target:self action:@selector(volumeChanged:)];
    _volume.frame = NSMakeRect(cs.width - 104, 3, 92, 16);
    _volume.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _volume.continuous = YES;
    [content addSubview:_volume];
    _player->setVolume(vol0);
    g_tapVolume = std::clamp(vol0 / 100.0f, 0.0f, 1.0f);

    // ---- transport cluster: ⏸ ⏪ [========] ⏩  "12:34 / 1:45:00" ----------------------------
    // Hidden until libVLC says the current media is actually seekable (see -updateTransport).
    // NB these are added BEFORE the split view below, so Video Only's full-bleed video covers
    // them by z-order as well as by the explicit hide in -applyVideoOnly.
    //
    // NO autoresizing masks: unlike everything else on this bar, the cluster's width depends on
    // whether it is SHOWN, so its geometry (and the status field's) is computed in
    // -layoutBottomBar and recomputed on every content-view resize and every visibility flip.
    _pauseBtn = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"pause.fill"
                                                   accessibilityDescription:Tr(StringId::TooltipBtnPause)]
                                   target:self action:@selector(togglePause:)];
    _pauseBtn.bordered = NO;
    _pauseBtn.toolTip = Tr(StringId::TooltipBtnPause);
    _pauseBtn.hidden = YES;
    [content addSubview:_pauseBtn];

    _skipBackBtn = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"gobackward.10"
                                                      accessibilityDescription:TrF(StringId::TooltipBtnSkipBack, {@"10"})]
                                      target:self action:@selector(skipBack:)];
    _skipBackBtn.bordered = NO;
    _skipBackBtn.toolTip = TrF(StringId::TooltipBtnSkipBack, {@"10"});
    _skipBackBtn.hidden = YES;
    [content addSubview:_skipBackBtn];

    _seek = [NSSlider sliderWithValue:0 minValue:0 maxValue:1
                               target:self action:@selector(seekChanged:)];
    // continuous so the thumb tracks the drag, but the SEEK itself only fires on mouse-up —
    // seeking a network stream re-buffers, so a per-tick seek would thrash the demuxer.
    _seek.continuous = YES;
    _seek.hidden = YES;
    [content addSubview:_seek];

    _skipFwdBtn = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"goforward.10"
                                                     accessibilityDescription:TrF(StringId::TooltipBtnSkipFwd, {@"10"})]
                                     target:self action:@selector(skipForward:)];
    _skipFwdBtn.bordered = NO;
    _skipFwdBtn.toolTip = TrF(StringId::TooltipBtnSkipFwd, {@"10"});
    _skipFwdBtn.hidden = YES;
    [content addSubview:_skipFwdBtn];

    _seekTime = [NSTextField labelWithString:@""];
    _seekTime.textColor = NSColor.secondaryLabelColor;
    _seekTime.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
    _seekTime.alignment = NSTextAlignmentRight;
    _seekTime.hidden = YES;
    [content addSubview:_seekTime];

    // The bottom bar is the ONE strip whose layout is not expressible with autoresizing masks,
    // because the status field's width depends on whether the transport cluster is present.
    // Observe the CONTENT VIEW's frame directly rather than piggy-backing on the video pane's
    // resize notification: this is a bottom-bar concern, and the video pane also moves when the
    // channel grid is shown/hidden, which has nothing to do with it.
    content.postsFrameChangedNotifications = YES;
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(contentFrameChanged:)
                                               name:NSViewFrameDidChangeNotification
                                             object:content];
    [self layoutBottomBar];

    // ---- split: channel grid | video ----
    NSSplitView* split = [[NSSplitView alloc]
        initWithFrame:NSMakeRect(0, statusH, cs.width, cs.height - barH - statusH)];
    _split = split;
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 380, 100)];
    scroll.hasVerticalScroller = YES;
    _table = [[RETableView alloc] initWithFrame:scroll.bounds];
    _table.target = self;
    _table.action = @selector(gridClicked:);            // single-click a ★ cell toggles favourite
    _table.doubleAction = @selector(playSelectedRow:);  // double-click / Return plays
    [self addColumn:@"fav" title:@"★" width:26];
    [self addColumn:@"num" title:@"#" width:46];
    // Channel-logo thumbnail column: an image cell fed an NSImage by -objectValueForTableColumn.
    NSTableColumn* logoCol = [[NSTableColumn alloc] initWithIdentifier:@"logo"];
    logoCol.title = @"";
    logoCol.width = 28;
    logoCol.minWidth = 20;
    logoCol.maxWidth = 40;
    NSImageCell* logoCell = [[NSImageCell alloc] init];
    logoCell.imageScaling = NSImageScaleProportionallyUpOrDown;
    logoCell.imageAlignment = NSImageAlignCenter;
    logoCol.dataCell = logoCell;
    [_table addTableColumn:logoCol];
    [self addColumn:@"name" title:Tr(StringId::LabelChannel) width:172];
    [self addColumn:@"group" title:Tr(StringId::GridHeaderGroup) width:120];
    _table.rowHeight = 24;  // taller rows so the logo thumbnails are legible
    _table.headerView = [[NSTableHeaderView alloc] init];
    _table.dataSource = self;
    _table.delegate = self;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.allowsColumnResizing = YES;
    _table.menu = [self makeRowMenu];  // right-click: Play / Toggle Favourite
    scroll.documentView = _table;
    [split addSubview:scroll];

    // Channel-logo cache. The disk cache lives beside the DB (honouring RABBITEARS_DATA_DIR),
    // under .../RabbitEars/logos. onReady fires on the main thread as images arrive -> a
    // coalesced reload of the visible logo cells (see -logosDidLoad).
    // A faint generic glyph shown while a logo loads and for logo-less channels (an image cell
    // does not auto-tint a template image, so bake the tint in via a source-atop fill — all
    // fully-declared AppKit APIs; NSImage has no -imageWithTintColor:). MRC: the returned image is
    // autoreleased, so RETAIN it into the app-lifetime ivar (else it dangles after this pool drains
    // and the first table draw over-releases it).
    NSImage* tv = [NSImage imageWithSystemSymbolName:@"tv" accessibilityDescription:nil];
    _logoPlaceholder = [[NSImage imageWithSize:tv.size flipped:NO drawingHandler:^BOOL(NSRect rect) {
        [tv drawInRect:rect];
        [NSColor.tertiaryLabelColor set];
        NSRectFillUsingOperation(rect, NSCompositingOperationSourceAtop);  // tint the glyph
        return YES;
    }] retain];
    NSString* dbPath = ns(Database::defaultDbPath());
    NSString* logosDir = [dbPath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"logos"];
    MainWindowController* __unsafe_unretained meLogos = self;  // app-lifetime; no retain cycle
    _logoLoader = [[LogoLoader alloc] initWithCacheDir:logosDir onReady:^{ [meLogos logosDidLoad]; }];

    // Right split pane: the video, with both meters on ONE fixed-width line pinned to
    // the bottom-left (siblings, not overlaid — no z-order fight with libVLC's surface).
    // The always-on stats meter is at the left; the opt-in spectrum meter sits to its
    // right when enabled. Each is a fixed kMeterW wide — they do NOT stretch with the
    // window; the video fills the space above. -applyMeterLayout positions them.
    NSView* videoPane = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 100)];
    _videoContainer = videoPane;  // holds the video panes + the floating meter bar
    _videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, kMeterH, 600, 100 - kMeterH)];
    _videoView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _videoView.wantsLayer = YES;
    _videoView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [videoPane addSubview:_videoView];
    _panes[0]->view = _videoView;  // pane 0's surface is the active video view (aliased above)
    _videoMenu = [[NSMenu alloc] init];  // MRC: app-lifetime; the ivar + each pane view's .menu hold it
    _videoMenu.delegate = self;          // -menuNeedsUpdate: fills it with live state on each right-click
    _videoMenu.autoenablesItems = NO;
    _videoView.menu = _videoMenu;        // right-click the video -> the Win32-parity view menu
    NSClickGestureRecognizer* dbl =
        [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(videoDoubleClicked:)];
    dbl.numberOfClicksRequired = 2;
    [_videoView addGestureRecognizer:dbl];
    [dbl release];  // MRC: the view retains it
    // Single-click activates this pane in multi-view (a harmless no-op in Single view).
    NSClickGestureRecognizer* act0 =
        [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(paneClicked:)];
    [_videoView addGestureRecognizer:act0];
    [act0 release];

    // The meters live in a draggable bar that floats over the video. -applyMeterLayout
    // lays out the enabled ones inside it and sizes + positions the bar.
    _meterBar = [[DraggableMeterBar alloc] initWithFrame:NSMakeRect(0, 0, 10, kMeterH)];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle
    _meterBar.onMoved = ^{ [me persistMeterPos]; [me persistMeterHeight]; };
    _meterBar.onResize = ^(CGFloat barH) { [me resizeMetersToBarHeight:barH]; };
    static const MeterKind meterKinds[4] = {MeterKind::Spectrum, MeterKind::Signal,
                                            MeterKind::Bitrate, MeterKind::Frames};
    for (int k = 0; k < 4; ++k) {
        _meters[k] = [[MeterView alloc] initWithKind:meterKinds[k]];
        _meters[k].hidden = YES;
        [_meterBar addSubview:_meters[k]];
        _meterLabelViews[k] = [[NSTextField labelWithString:@""] retain];  // MRC: app-lifetime ivar
        _meterLabelViews[k].hidden = YES;
        [_meterBar addSubview:_meterLabelViews[k]];
    }
    _tank = [[TankView alloc] initWithFrame:NSMakeRect(0, 0, 92, kMeterH)];  // MRC: app-lifetime, +1 held
    _tank.hidden = YES;
    _tank.onHideRequested = ^{ [me setDataFlowOn:NO]; };
    [_meterBar addSubview:_tank];
    _tankLabel = [[NSTextField labelWithString:@""] retain];
    _tankLabel.hidden = YES;
    [_meterBar addSubview:_tankLabel];
    [videoPane addSubview:_meterBar];  // sibling of the video, on top (floats)
    videoPane.postsFrameChangedNotifications = YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(videoPaneResized:)
                                               name:NSViewFrameDidChangeNotification object:videoPane];
    [split addSubview:videoPane];

    // Empty-state hint, centered over the (black) video pane until channels exist.
    _emptyHint = [NSTextField labelWithString:Tr(StringId::StatusNoChannelsYet)];
    _emptyHint.textColor = NSColor.secondaryLabelColor;
    _emptyHint.font = [NSFont systemFontOfSize:15];
    _emptyHint.translatesAutoresizingMaskIntoConstraints = NO;
    [_videoView addSubview:_emptyHint];
    [NSLayoutConstraint activateConstraints:@[
        [_emptyHint.centerXAnchor constraintEqualToAnchor:_videoView.centerXAnchor],
        [_emptyHint.centerYAnchor constraintEqualToAnchor:_videoView.centerYAnchor],
    ]];

    split.delegate = self;  // keep the grid a fixed width; the video pane fills the rest
    [content addSubview:split];
    _gridWidth = 380;
    [self layoutSplitPanes:split];  // fill now — no blank strip before the first resize
    [self applyVideoPaneLayout];    // position pane 0 to fill (owns pane frames now)
    // The window + all its chrome are built — SHOW IT before any launch-time modal, so the app
    // always has a visible, front window. (A Sparkle post-update relaunch may not activate the
    // app; the old app-modal Terms panel then came up buried with no main window, reading as a
    // hang/beachball.)
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // Terms-of-Use gate (peer of the Win32 gate): re-accept on every version change (fresh
    // install OR update); `tos_accepted` stores the full version last accepted. Presented as a
    // SHEET on the now-visible window (a sheet cannot be buried independently of its window).
    // Declining quits; the "usage" side effects (playback, tap, scheduler) start only on Accept.
    if (_db) {
        const std::wstring tosVer = RE_VERSION_FULL_W;
        const auto accepted = _db->getSetting(L"tos_accepted");
        if (!accepted || *accepted != tosVer) {
            TermsDialog* terms = [[TermsDialog alloc] initWithVersion:ns(RE_VERSION_W)];
            MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle
            [terms beginSheetForWindow:_window completion:^(BOOL ok) {
                if (!ok) { diag::info(L"Terms declined — exiting"); [NSApp terminate:nil]; return; }
                if (me->_db) me->_db->setSetting(L"tos_accepted", tosVer);
                diag::info(L"Terms accepted for " + tosVer);
                [me finishStartup];
            }];
            [terms release];  // MRC: the sheet + its completion block keep the dialog alive
            return;           // -finishStartup runs from the completion handler after Accept
        }
    }
    [self finishStartup];  // already accepted (or no DB) — start up immediately
}

// Post-Terms-of-Use startup: the "usage" side effects, split out of -showWindow so they run
// only after the ToU sheet is accepted (or immediately when no gate is needed) — never behind
// a hidden modal on a non-activating launch.
- (void)finishStartup {
    _startupFinished = YES;         // the app is now usable — re-enables the gated menu commands
    [self applyMeterConfig];        // configure + show the enabled meters (start the tap if Spectrum is on)
    if (_db) if (auto b = _db->getSetting(L"meter_bridge"); b && *b == L"1") [self openMeterBridge];  // as left

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into

    // Poll libVLC stream stats every 250ms to drive the meter (no audio capture).
    _bitrateMax = 0.0;
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self
                                                 selector:@selector(tickStats) userInfo:nil repeats:YES];

    [self restoreLastPlaylist];
    [self startScheduler];  // begin the ~30s recording-scheduler tick
}

// ---- multi-view pane accessors ----
- (MacVideoPane*)activePane {
    if (_activePane < 0 || _activePane >= (int)_panes.size()) return nullptr;
    return _panes[(size_t)_activePane].get();
}
- (VlcPlayerMac*)activePlayer {
    MacVideoPane* p = [self activePane];
    return p ? p->player.get() : nullptr;
}

- (void)addColumn:(NSString*)ident title:(NSString*)title width:(CGFloat)w {
    NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ident];
    c.title = title;
    c.width = w;
    c.minWidth = 20;
    [_table addTableColumn:c];
}

// ---- split layout (channel grid | video) ----

// The channel grid keeps _gridWidth; the video pane fills the rest. Called on every
// split resize and once at setup, so the video always fills — no blank strip before
// the first resize.
- (void)layoutSplitPanes:(NSSplitView*)sv {
    if (sv.subviews.count < 2) return;
    const CGFloat W = NSWidth(sv.bounds), H = NSHeight(sv.bounds), d = sv.dividerThickness;
    sv.subviews[0].hidden = _gridHidden || _videoOnly;
    if (_gridHidden || _videoOnly) {  // channel list collapsed — video fills the pane, no divider gap
        sv.subviews[0].frame = NSMakeRect(0, 0, 0, H);
        sv.subviews[1].frame = NSMakeRect(0, 0, W, H);
        return;
    }
    const CGFloat left = MAX(160, MIN(_gridWidth, W - d - 200));  // keep both panes usable
    sv.subviews[0].frame = NSMakeRect(0, 0, left, H);
    sv.subviews[1].frame = NSMakeRect(left + d, 0, W - left - d, H);
}

- (void)splitView:(NSSplitView*)sv resizeSubviewsWithOldSize:(NSSize)__unused oldSize {
    [self layoutSplitPanes:sv];  // window resize grows the video, not the grid
}

// Remember a divider the user dragged, so later window resizes preserve it.
- (void)splitViewDidResizeSubviews:(NSNotification*)note {
    if (_gridHidden) return;  // don't remember the collapsed (0) width
    NSSplitView* sv = note.object;
    if (sv.subviews.count >= 1) _gridWidth = NSWidth(sv.subviews[0].frame);
}

- (CGFloat)splitView:(NSSplitView*)__unused sv
    constrainMinCoordinate:(CGFloat)__unused m ofSubviewAt:(NSInteger)__unused i { return 160; }

- (CGFloat)splitView:(NSSplitView*)sv
    constrainMaxCoordinate:(CGFloat)__unused m ofSubviewAt:(NSInteger)__unused i {
    return NSWidth(sv.bounds) - 200;
}

// ---- View-menu chrome toggles (hide the channel list / top toolbar so the video
//      fills the window). The bottom status/volume bar is left in place. ----

- (BOOL)channelListHidden { return _gridHidden; }
- (BOOL)toolbarHidden { return _toolbarHidden; }
- (BOOL)startupFinished { return _startupFinished; }

- (void)toggleChannelList {
    _gridHidden = !_gridHidden;
    [self layoutSplitPanes:_split];
}

- (void)toggleToolbar {
    _toolbarHidden = !_toolbarHidden;
    _topBar.hidden = _toolbarHidden;
    // Grow the split up into the freed bar space (or back down when shown); the
    // split's autoresizing then keeps that margin across later window resizes.
    NSView* content = _window.contentView;
    const CGFloat W = content.bounds.size.width, H = content.bounds.size.height;
    const CGFloat topEdge = _toolbarHidden ? H : H - kBarH;
    _split.frame = NSMakeRect(0, kStatusH, W, topEdge - kStatusH);
}

// ---- Video Only (View ▸ Video Only) ------------------------------------------
// Collapse ALL chrome — toolbar, channel list, the meters, and the bottom volume/
// status bar — so the video fills the window; toggling back restores the prior chrome
// state. Esc or a double-click on the video exits (mirrors the Win32 video-only mode).
- (BOOL)videoOnly { return _videoOnly; }
- (void)toggleVideoOnly { _videoOnly = !_videoOnly; [self applyVideoOnly]; }

- (void)applyVideoOnly {
    const BOOL v = _videoOnly;
    _topBar.hidden = v || _toolbarHidden;
    _status.hidden = v;
    _muteBtn.hidden = v;
    _volume.hidden = v;
    // TWO conditions, unlike the always-wanted controls above: the transport cluster has its own
    // `want` (_seekShown). A single-condition `= v` would un-hide a scrub bar on a live channel
    // when the user LEAVES Video Only, and it would stay up until a tick re-hid it.
    _meterBtn.hidden     = v;   // (was missing entirely — it only vanished by z-order before)
    _pauseBtn.hidden     = v || !_seekShown;
    _skipBackBtn.hidden  = v || !_seekShown;
    _seek.hidden         = v || !_seekShown;
    _skipFwdBtn.hidden   = v || !_seekShown;
    _seekTime.hidden     = v || !_seekShown;

    NSView* content = _window.contentView;
    const CGFloat W = content.bounds.size.width, H = content.bounds.size.height;
    const CGFloat topEdge = (v || _toolbarHidden) ? H : H - kBarH;
    const CGFloat botEdge = v ? 0 : kStatusH;
    _split.frame = NSMakeRect(0, botEdge, W, topEdge - botEdge);
    [self layoutSplitPanes:_split];  // grid stays hidden while video-only
    [self applyMeterLayout];         // hides the meters + fills the video while video-only

    if (v) [self installEscMonitor];
    else   [self removeEscMonitor];
    [self updateDisplaySleepAssertion];  // Video Only is an immersive full-screen viewing mode
}

// Suspend the screen saver / display idle-sleep while the user is in an immersive viewing mode
// (native full screen or Video Only), so a movie isn't interrupted. A SEPARATE assertion from the
// recording keep-awake (_keepAwake, which prevents SYSTEM idle-sleep) — this one prevents DISPLAY
// idle-sleep (which also blocks the screen saver). Released the moment we leave the immersive mode.
- (void)updateDisplaySleepAssertion {
    const BOOL immersive = ((_window.styleMask & NSWindowStyleMaskFullScreen) != 0) || _videoOnly;
    if (immersive && _keepDisplayAwake == 0) {
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                    kIOPMAssertionLevelOn, CFSTR("RabbitEars full-screen playback"),
                                    &_keepDisplayAwake);
    } else if (!immersive && _keepDisplayAwake != 0) {
        IOPMAssertionRelease(_keepDisplayAwake);
        _keepDisplayAwake = 0;
    }
}

// NSWindowDelegate — native full-screen enter/exit drives the display-sleep assertion above.
- (void)windowDidEnterFullScreen:(NSNotification*)__unused n { [self updateDisplaySleepAssertion]; }
- (void)windowDidExitFullScreen:(NSNotification*)__unused n  { [self updateDisplaySleepAssertion]; }

// NSMenuDelegate — rebuild the video right-click menu fresh on each open so its checkmarks track the
// live state (the mac peer of the Win32 WM_RBUTTONUP view menu: Video Only / Fullscreen / view modes).
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != _videoMenu) return;
    [menu removeAllItems];

    // PiP-specific items FIRST, and only when the right-click actually landed on the inset. One
    // NSMenu is shared by every pane (each pane view's .menu points at it), so the pane has to be
    // recovered from the event that opened the menu — see -paneIndexForCurrentEvent.
    //
    // ⚠ Win32's third PiP item, "Keep PIP above other apps", is deliberately NOT here. It is a
    // z-order policy for a separate top-level PiP WINDOW, and mac's PiP is not one: the inset is an
    // ordinary subview of _videoContainer inside the single main window (this controller creates
    // exactly one NSWindow), so it is already above the backdrop by z-order and there is nothing
    // for the setting to control. The literal analogue would be floating the WHOLE main window
    // above other apps, which is a different feature under a misleading name. N/A by design, like
    // the theme engine and wake-to-record.
    if (_viewMode == ViewMode::Pip && _panes.size() >= 2 && [self paneIndexForCurrentEvent] > 0) {
        [[menu addItemWithTitle:Tr(StringId::MenuPipSwapWithMain)
                         action:@selector(swapPipWithMain:) keyEquivalent:@""] setTarget:self];
        [[menu addItemWithTitle:Tr(StringId::MenuPipClose)
                         action:@selector(closePip:) keyEquivalent:@""] setTarget:self];
        [menu addItem:[NSMenuItem separatorItem]];
    }

    NSMenuItem* vo = [menu addItemWithTitle:Tr(StringId::MenuVideoOnlyPlain)
                                     action:@selector(toggleVideoOnly:) keyEquivalent:@""];
    vo.target = NSApp.delegate;  // the AppDelegate owns Video Only (also the View menu-bar item)
    vo.state = _videoOnly ? NSControlStateValueOn : NSControlStateValueOff;
    NSMenuItem* fs = [menu addItemWithTitle:Tr(StringId::Fullscreen)
                                     action:@selector(toggleFullScreen:) keyEquivalent:@""];
    fs.target = _window;  // NSWindow handles native full screen (⌃⌘F / green button)
    fs.state = ((_window.styleMask & NSWindowStyleMaskFullScreen) != 0)
                   ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:[NSMenuItem separatorItem]];
    struct { NSString* title; SEL sel; ViewMode vm; } rows[] = {
        { Tr(StringId::MenuSingleView),       @selector(setViewSingle:), ViewMode::Single },
        { Tr(StringId::MenuSplitView),        @selector(setViewSplit:),  ViewMode::Split  },
        { Tr(StringId::MenuPictureInPicture), @selector(setViewPip:),    ViewMode::Pip    } };
    for (auto& r : rows) {
        NSMenuItem* it = [menu addItemWithTitle:r.title action:r.sel keyEquivalent:@""];
        it.target = self;
        it.state = (_viewMode == r.vm) ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

- (void)videoDoubleClicked:(id)__unused g {
    if (_videoOnly) [self toggleVideoOnly];  // double-click exits video-only (Win32 parity)
}

// Which pane did the event that opened the context menu land on? Returns -1 when it cannot be
// determined. Topmost-first, because the PiP inset overlaps pane 0 and NSView subview order puts
// later siblings on top — scanning forwards would always answer "pane 0".
- (int)paneIndexForCurrentEvent {
    NSEvent* e = NSApp.currentEvent;
    if (!e || !_videoContainer || _panes.empty()) return -1;
    const NSPoint p = [_videoContainer convertPoint:e.locationInWindow fromView:nil];
    for (int i = (int)_panes.size() - 1; i >= 0; --i) {
        NSView* v = _panes[(size_t)i]->view;
        if (v && NSPointInRect(p, v.frame)) return i;
    }
    return -1;
}

// Promote the inset to the main view and demote the main view into the inset.
//
// ⚠ ACTIVATE PANE 0 BEFORE THE RE-OPENS, not after. -playChannel:intoPane: persists
// `last_channel_id` only for the pane that is active AT THE TIME, and clicking the inset makes it
// active — so doing this afterwards records the channel being DEMOTED, and "Resume last channel"
// then comes back to the wrong one on the next launch. Win32 hit exactly this and fixed it the
// same way (Win32/ui/MainWindowCommands.cpp swapPipWithMain). Audio follows the main view either
// way: promoting the inset means the thing you now watch big is the thing you hear.
- (void)swapPipWithMain:(id)__unused sender {
    if (_viewMode != ViewMode::Pip || _panes.size() < 2) return;
    // VALUE copies taken up front: the re-opens below assign through _panes[…]->channel, so a
    // reference into the vector would be read after it had already been overwritten.
    const Channel mainCh = _panes[0]->channel;
    const Channel pipCh  = _panes[1]->channel;
    const long long mainId = _panes[0]->channelId, pipId = _panes[1]->channelId;
    if (mainId == 0 && pipId == 0) return;   // both empty — nothing to swap

    [self setActivePane:0];

    // An empty side must be STOPPED, not "played": handing -playChannel:intoPane: a default
    // Channel would hand libVLC an empty URL. Clearing the bookkeeping too is what stops the grid
    // highlight — and a later swap — from believing a channel still lives there.
    if (pipId != 0) {
        [self playChannel:pipCh intoPane:0];
    } else {
        _panes[0]->player->stop(); _panes[0]->channel = Channel{}; _panes[0]->channelId = 0;
    }
    if (mainId != 0) {
        [self playChannel:mainCh intoPane:1];
    } else {
        _panes[1]->player->stop(); _panes[1]->channel = Channel{}; _panes[1]->channelId = 0;
    }

    [self updateEmptyHint];   // whichever side just went empty has to show its hint again
    [self setStatus:Tr(StringId::StatusPipSwapped)];
    diag::info(L"PIP swap: main #" + std::to_wstring(mainId) + L" <-> pip #" +
               std::to_wstring(pipId));
}

// Close the inset by collapsing to Single. -setViewSingle: already carries the pane-collapse rules
// (which pane survives, cancelling the provider-facing workers), so this must not reimplement them.
- (void)closePip:(id)sender { [self setViewSingle:sender]; }

// Esc exits video-only. A local monitor is installed only while video-only is active,
// so Esc behaves normally otherwise.
- (void)installEscMonitor {
    if (_escMonitor) return;
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no block retain cycle (MRC)
    _escMonitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                         handler:^NSEvent*(NSEvent* e) {
        if (e.keyCode == 53 && me->_videoOnly) {  // 53 = Esc
            [me toggleVideoOnly];
            return nil;  // consume
        }
        return e;
    }] retain];  // MRC: the returned monitor is autoreleased — hold it until -removeEscMonitor
}

- (void)removeEscMonitor {
    if (!_escMonitor) return;
    [NSEvent removeMonitor:_escMonitor];
    [_escMonitor release];
    _escMonitor = nil;
}

- (NSMenu*)makeRowMenu {
    NSMenu* m = [[NSMenu alloc] init];
    [[m addItemWithTitle:Tr(StringId::LabelPlay) action:@selector(playClicked:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MenuChannelPlayInPip) action:@selector(playInPip:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MacMainWindowToggleFavourite) action:@selector(toggleFavourite:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MenuChannelShowInGuide) action:@selector(showInGuide:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MacMainWindowSetChannelNumber) action:@selector(editChannelNumber:) keyEquivalent:@""] setTarget:self];
    return m;
}

// The truncation notice is STICKY — appended to EVERY status message while the grid is capped,
// rather than written once by -refreshChannels. The status line is a single shared slot and
// several flows overwrite it on the very next statement after refilling the grid; the worst is a
// playlist import reporting "Added 411,149 channels" over a grid holding 5,000 of them. A message
// that can be clobbered by the one event most likely to cause truncation is not a message.
// (Win32 does the same in its setStatus — MainWindow.cpp:105-111.)
- (void)setStatus:(NSString*)s {
    _status.stringValue =
        _gridTruncated
            ? [s stringByAppendingString:TrF(StringId::StatusGridTruncatedSuffix,
                                             {[NSString stringWithFormat:@"%d", kMaxGridRows]})]
            : s;
}

// Settings ▸ Language applies LIVE (no restart), the mac peer of Win32 applyLanguageChange. The
// caller (AppDelegate -selectLanguage:) has already flipped the process-global active language via
// i18n::setActiveLang, so every Tr()/TrF() below reads the NEW language. Relabel every built-once
// surface in place — AppKit controls redraw when their title/stringValue/placeholderString/toolTip
// changes. Menus/sheets built fresh on open (the gear pull-down, every NSAlert) and views that
// re-read Tr() in drawRect: (MeterView, EpgGuideView) need only a repaint. No font work: the macOS
// system font cascades to CJK on its own. Main thread only (menu-action driven).
- (void)applyLanguageLive {
    // Window title — re-derive from the active pane's channel (not a hardcoded app name).
    MacVideoPane* ap = [self activePane];
    _window.title = (ap && ap->channelId)
        ? TrF(StringId::MacMainWindowTitleWithChannel, {ns(ap->channel.name)})
        : Tr(StringId::AppName);

    // Top bar.
    _addBtn.title = Tr(StringId::CmdAddPlaylist);
    _setBtn.image = [NSImage imageWithSystemSymbolName:@"gearshape"
                                accessibilityDescription:Tr(StringId::TooltipSettings)];
    _setBtn.toolTip = Tr(StringId::TooltipSettings);
    _stopBtn.title = Tr(StringId::MacMainWindowStopButton);
    [self updateRecordButton];  // _recBtn image + toolTip, re-read from the live recording state
    // _muteBtn is an emoji glyph (🔊/🔇, language-independent) — leave it.
    _meterBtn.image = [NSImage imageWithSystemSymbolName:@"waveform"
                                 accessibilityDescription:Tr(StringId::MacMainWindowToggleMetersTooltip)];
    _meterBtn.toolTip = Tr(StringId::MacMainWindowToggleMetersTooltip);
    // Transport cluster. _pauseBtn is NOT here on purpose — -updateTransport rebuilds its image
    // and tooltip on every 250 ms tick, so it re-localizes itself. The two skip buttons are
    // built ONCE at setup, so without this they kept their launch-language tooltip AND their
    // launch-language accessibility description (VoiceOver) for the rest of the process.
    _skipBackBtn.image = [NSImage imageWithSystemSymbolName:@"gobackward.10"
                                    accessibilityDescription:TrF(StringId::TooltipBtnSkipBack, {@"10"})];
    _skipBackBtn.toolTip = TrF(StringId::TooltipBtnSkipBack, {@"10"});
    _skipFwdBtn.image  = [NSImage imageWithSystemSymbolName:@"goforward.10"
                                    accessibilityDescription:TrF(StringId::TooltipBtnSkipFwd, {@"10"})];
    _skipFwdBtn.toolTip = TrF(StringId::TooltipBtnSkipFwd, {@"10"});
    _search.placeholderString = Tr(StringId::SearchChannelsPlaceholder);  // NOT stringValue (user text)

    // Channel-grid column headers (the ★/# glyph columns are language-independent — leave them).
    [_table tableColumnWithIdentifier:@"name"].title = Tr(StringId::LabelChannel);
    [_table tableColumnWithIdentifier:@"group"].title = Tr(StringId::GridHeaderGroup);
    [_table.headerView setNeedsDisplay:YES];

    // Right-click row menu — attached once at setup, never rebuilt on open, so relabel it wholesale.
    NSMenu* rm = [self makeRowMenu];  // +1 (alloc, NOT autoreleased)
    _table.menu = rm;                  // the retain property releases the old menu + retains rm
    [rm release];                      // balance the +1, else a leak per switch

    // Empty-pane hint (updateEmptyHint only toggles .hidden; it never re-sets the text).
    _emptyHint.stringValue = Tr(StringId::StatusNoChannelsYet);

    // Filter popup — rebuildFilterMenu re-reads its Tr labels but force-selects index 0; preserve
    // the user's current filter across the rebuild. Group/country/VOD-category names ride on
    // representedObject as DATA, untranslated, so they survive a language switch unchanged.
    [self rebuildFilterMenuPreservingSelection];

    // Status line — best-effort re-derive (a transient in-progress message can't be reconstructed;
    // it re-renders in the new language on the next event). Playing → StatusPlaying, else the count.
    if (ap && ap->channelId)
        [self setStatus:TrF(StringId::StatusPlaying, {ns(ap->channel.name)})];
    else
        // On the 🎬 Movies section head the count is meaningless (it deliberately loads nothing),
        // so re-derive the prompt rather than telling the user they have "0 channels".
        [self setStatus:_filter.selectedItem.tag == kFilterMovies
                            ? Tr(StringId::StatusMoviesPickCategory)
                            : TrF(StringId::MacMainWindowChannelCountStatus,
                                  {[NSString stringWithFormat:@"%lu", (unsigned long)_channels.size()],
                                   _search.stringValue.length ? Tr(StringId::MacMainWindowSearchSuffix) : @""})];

    // Views that re-read Tr() in drawRect: — a repaint refreshes their text; the meter also caches a
    // hover toolTip, so it gets a dedicated relabel entry point.
    for (int i = 0; i < 4; ++i) [_meters[i] relabelForLanguageChange];

    // Modeless windows are lazily created + REUSED (nil until first opened). Relabel in place if
    // present — never nil-and-rebuild while visible (would dangle their self-referencing
    // dataSource/delegate/target back-refs → message-to-freed crash).
    if (_recordingsWC) [(RecordingsWindowController*)_recordingsWC relabelForLanguageChange];
    if (_guideWC)      [(TvGuideWindowController*)_guideWC relabelForLanguageChange];
    [_meterBridge relabelForLanguageChange];  // the bridge's title + labels
    [_tank relabelForLanguageChange];         // its right-click "Hide buffer fluid"
    [self applyMeterLayout];                  // the tray's labels re-read their text
}

// ---- playlist load / filter model ----

- (void)restoreLastPlaylist {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailableRestart)]; return; }
    const auto playlists = _db->listPlaylists();
    long long pid = 0;
    for (const auto& p : playlists) if (p.enabled) pid = p.id;  // last enabled (added_at order)
    if (!pid) {
        [self setStatus:Tr(playlists.empty()
                            ? StringId::MacMainWindowReadyLoadHint
                            : StringId::MacMainWindowAllPlaylistsDisabledManage)];
        [self rebuildFilterMenu];
        return;
    }
    [self showPlaylist:pid];
    [self selectLastPlayed];
}

// The channel that was playing when the app last quit (saved as "last_channel_id" on every play).
// On launch either RESUME it (auto-play — Win32 parity, when "resume_last" is on) or just highlight
// it with a "double-click to resume" hint. The row is selected/scrolled-to when it's in the current
// view; auto-play resolves by id via channelById so a channel outside the current filter/playlist
// still resumes (channelById returns nullopt if the channel/playlist was deleted — then do nothing).
- (void)selectLastPlayed {
    if (!_db) return;
    const auto s = _db->getSetting(L"last_channel_id");
    if (!s || s->empty()) return;
    const long long cid = std::wcstoll(s->c_str(), nullptr, 10);

    NSInteger vis = -1;
    for (size_t i = 0; i < _channels.size(); ++i)
        if (_channels[i].id == cid) { vis = (NSInteger)i; break; }
    if (vis >= 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)vis] byExtendingSelection:NO];
        [_table scrollRowToVisible:vis];
    }

    if (_resumeLast) {
        if (auto ch = _db->channelById(cid)) [self playChannel:*ch];  // auto-play into the active pane
    } else if (vis >= 0) {
        [self setStatus:TrF(StringId::MacMainWindowLastPlayedResume, {ns(_channels[(size_t)vis].name)})];
    }
}

// Settings ⚙ ▸ Channels ▸ Resume last channel — toggle auto-play-on-launch (persists "resume_last").
- (void)toggleResumeLast:(id)__unused sender {
    _resumeLast = !_resumeLast;
    if (_db) _db->setSetting(L"resume_last", _resumeLast ? L"1" : L"0");
}

// Make `pid` the active playlist: reset search/filter, rebuild groups, reload.
- (void)showPlaylist:(long long)pid {
    _currentPid = pid;
    _search.stringValue = @"";
    [self rebuildFilterMenu];       // selects "All channels"
    [self refreshChannels];
}

// Build items manually (NOT addItemWithTitle, which de-dupes by title) so a real
// M3U group named "All channels"/"★ Favourites" can't clobber a reserved item;
// the true group name rides on representedObject and routing keys off tag +
// representedObject, never the display title.
- (void)addFilterItem:(NSString*)title tag:(NSInteger)tag group:(NSString*)group {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    it.tag = tag;
    it.representedObject = group;
    [_filter.menu addItem:it];  // the menu retains it
    [it release];               // balance the +1 alloc (MRC) — else a leak per item, per rebuild
}

// Rebuild the nav popup and put the user's selection back. THE ONE implementation of this dance —
// it existed twice inline (language switch, playlist reload) and the VOD sync needed a third, and
// this is the exact code where an MRC use-after-free shipped once already: [removeAllItems]
// releases the old items, each of which owns its representedObject string, so a bare pointer read
// after the rebuild dangles. Retain+autorelease across the rebuild is the fix; keeping it in one
// place is what stops the next caller re-deriving it wrongly.
- (void)rebuildFilterMenuPreservingSelection {
    NSMenuItem* prev = _filter.selectedItem;
    const NSInteger prevTag = prev ? prev.tag : kFilterAll;
    NSString* const prevRep = prev ? [[(NSString*)prev.representedObject retain] autorelease] : nil;
    [self rebuildFilterMenu];
    for (NSMenuItem* it in _filter.itemArray) {
        if (it.isSeparatorItem) continue;
        NSString* rep = (NSString*)it.representedObject;
        const BOOL repEq = (!rep && !prevRep) || (rep && prevRep && [rep isEqualToString:prevRep]);
        if (it.tag == prevTag && repEq) { [_filter selectItem:it]; break; }
    }
}

- (void)rebuildFilterMenu {
    [_filter removeAllItems];
    [self addFilterItem:Tr(StringId::NavAllChannels) tag:kFilterAll group:nil];
    [self addFilterItem:Tr(StringId::NavFavourites) tag:kFilterFavourites group:nil];
    if (_db) {
        const auto groups = _db->listGroups();
        if (!groups.empty()) [_filter.menu addItem:[NSMenuItem separatorItem]];
        for (const auto& g : groups) {
            if (g.empty()) continue;
            [self addFilterItem:ns(g) tag:kFilterGroup group:ns(g)];
        }
        const auto countries = _db->listCountries();
        if (!countries.empty()) [_filter.menu addItem:[NSMenuItem separatorItem]];
        for (const auto& cc : countries) {
            if (cc.empty()) continue;
            [self addFilterItem:TrF(StringId::MacMainWindowCountryFilterItem, {ns(cc).uppercaseString})
                            tag:kFilterCountry group:ns(cc)];
        }
        // 🎬 Movies — its own nav ROOT, not ~67 extra siblings in the live group list. Present
        // only when a VOD catalogue actually exists, so a user with a plain .m3u never sees a
        // section that could do nothing. listVodGroups() is kind-scoped (kind<>0) and listGroups()
        // above is LIVE-only, so the two trees are separate namespaces and a VOD category named
        // "News" cannot merge with a live group named "News".
        const auto vod = _db->listVodGroups();
        if (!vod.empty()) {
            [_filter.menu addItem:[NSMenuItem separatorItem]];
            [self addFilterItem:Tr(StringId::NavMovies) tag:kFilterMovies group:nil];
            for (const auto& g : vod) {
                if (g.empty()) continue;
                // Indented so a VOD category reads as a child of Movies in a FLAT popup — mac's
                // nav is an NSPopUpButton, not Win32's tree, so indentation is the only available
                // hierarchy cue. The representedObject stays the RAW group name (no prefix), since
                // it is passed straight to moviesByGroup().
                [self addFilterItem:[@"    " stringByAppendingString:ns(g)]
                                tag:kFilterMovieGroup group:ns(g)];
            }
        }
    }
    // Rebuild happens only on playlist load — always reset to "All channels" so a
    // switch/import shows the new playlist in full, never a stale group filter.
    [_filter selectItemAtIndex:0];
}

// Build the DAO filter from the current view settings (peer of Win32 gridFilter()). `capped` asks
// for kMaxGridRows + 1 rows; pass NO for any caller that needs a COMPLETE list.
- (rabbitears::Database::GridFilter)gridFilterCapped:(BOOL)capped {
    rabbitears::Database::GridFilter g;
    g.hideDead = _hideDead ? true : false;
    if (!_categoryFilter.empty())
        g.categories.assign(_categoryFilter.begin(), _categoryFilter.end());
    if (capped) g.limit = kMaxGridRows + 1;
    return g;
}

// Trim the +1 probe row and report whether it was there. Beside -gridFilterCapped: so the two
// halves of the trick cannot drift apart.
static bool trimToGridCap(std::vector<Channel>& ch) {
    if ((int)ch.size() <= kMaxGridRows) return false;
    ch.resize(kMaxGridRows);
    return true;
}

// Apply the current search text + filter selection and reload the table.
- (void)refreshChannels {
    // Cancel any pending debounced search. Done HERE rather than at each call site so that every
    // path into a refresh — filter popup, Categories apply, playlist reload, dead-status clear —
    // is safe from a keystroke fired 200 ms ago landing on top of it.
    // AUTOrelease, not release: this method is called from INSIDE the search timer's own block, so
    // a plain release can drop the last reference to the timer that is currently firing — and the
    // timer owns the block still executing. Deferring to the pool keeps it alive past the return.
    [_searchTimer invalidate];
    [_searchTimer autorelease];
    _searchTimer = nil;
    if (!_db) { _channels.clear(); _gridTruncated = NO; [_table reloadData]; return; }
    const std::wstring q = ws(_search.stringValue);
    NSMenuItem* sel = _filter.selectedItem;

    // ⚠ THE VIEW FILTERS LIVE IN SQL NOW, AND THAT IS THE WHOLE POINT. They used to be applied in
    // C++ over the returned vector, which composes the wrong way round the moment a limit exists:
    // "the matches among the first 5,000 rows" instead of "the first 5,000 matches". With a
    // Sports block sitting at position ~15,000 an include-filter of {Sports} would have returned
    // an EMPTY grid while Groups ▸ Sports showed the same channels perfectly. Pushing the
    // predicates down means the cap applies to the FILTERED set — see Database.h.
    const rabbitears::Database::GridFilter g = [self gridFilterCapped:YES];

    // 🎬 The Movies SECTION HEAD deliberately loads nothing. allMovies() is 40k+ rows on a real
    // provider, and a nav click must never be that — the cap would hide most of it anyway and the
    // user would be staring at an arbitrary 5,000. Prompt for a category instead.
    // Only when the box is EMPTY: a search term must still do something. Falling through with a
    // term runs the normal global search below, which is far better than a search box that
    // silently does nothing because of which nav row happens to be selected.
    if (sel.tag == kFilterMovies && q.empty()) {
        _channels.clear();
        _gridTruncated = NO;
        [_table deselectAll:nil];
        [_table reloadData];
        [self setStatus:Tr(StringId::StatusMoviesPickCategory)];
        [self updateEmptyHint];
        return;
    }

    // The channel set for the active filter (favourites / group / country / movie category / all).
    auto filteredSet = [&](const rabbitears::Database::GridFilter& gf) -> std::vector<Channel> {
        switch (sel.tag) {
            case kFilterFavourites: return _db->favourites(gf);
            case kFilterGroup:      return _db->channelsByGroup(ws(sel.representedObject), gf);
            case kFilterCountry:    return _db->channelsByCountry(ws(sel.representedObject), gf);
            case kFilterMovieGroup: return _db->moviesByGroup(ws(sel.representedObject), gf);
            // NB kFilterMovies never reaches here (early return above). Everything else — and
            // ONLY kFilterAll actually remains — falls through to the whole-library view.
            default:                return _currentPid ? _db->channelsByPlaylist(_currentPid, gf)
                                                       : _db->allChannels(gf);
        }
    };

    if (q.empty()) {
        _channels = filteredSet(g);
        _gridTruncated = trimToGridCap(_channels);
    } else if (sel.tag == kFilterAll || sel.tag == kFilterMovies) {
        // kFilterMovies joins kFilterAll here deliberately: the Movies row is a section HEAD with
        // no channel set of its own, so a search from it means "search everything". Routing it
        // down the capped single-query path also keeps it off the uncapped intersect below, which
        // would otherwise materialise the entire library just to build a membership set.
        _channels = _db->searchChannels(q, g);  // search across the full (all-channels) view
        _gridTruncated = trimToGridCap(_channels);
    } else {
        // Search AND filter both apply: intersect the search hits with the filter set by id, so
        // e.g. ★ Favourites + "news" shows favourites matching "news". The MEMBERSHIP set must be
        // UNCAPPED — capping it would drop a genuine hit merely for sitting past row 5,000 of the
        // filter view — so the cap is applied to the intersection afterwards, which is the only
        // place it means "the first N results".
        //
        // ⚠ BOTH HALVES HERE ARE UNCAPPED, AND THIS IS THE ONE GRID PATH kMaxGridRows DOES NOT
        // COVER. GridFilter carries only {limit, hideDead, categories} — it cannot express the
        // favourites/group/country predicate — so `searchChannels` below is a GLOBAL name search
        // over the whole library, and gridLimit() emits nothing for limit 0 (a deliberate choice:
        // SQLite's "LIMIT 0" returns no rows). Since schema v11 that search goes through the FTS5
        // channel-name index (docs/CHANNEL_SEARCH.md — 0.2–1 ms for most words on 410k channels,
        // ~45 ms for the commonest; under 3 characters a LIKE scan of the names, ~110–150 ms); it
        // was a triple-LIKE over name/group/tvg-name, measured ~1.6 s at 411k rows, before.
        //
        // DO NOT "fix" this by capping the search half: that yields the filter-matches among the
        // first 5,000 GLOBAL hits — exactly the compose-the-wrong-way-round bug Database.h:28-36
        // warns about (a verified example turned 8,000 correct rows into 100, with no truncation
        // notice, because the intersection hides it). The real fix is to push the view predicate
        // (and ideally the search term) INTO GridFilter so one capped statement does the whole
        // job and this branch disappears. Left for the release that touches this area — it is
        // pre-existing, not a regression of the pushdown.
        std::set<long long> keep;
        for (const auto& c : filteredSet([self gridFilterCapped:NO])) keep.insert(c.id);
        _channels.clear();
        for (auto& c : _db->searchChannels(q, [self gridFilterCapped:NO]))
            if (keep.count(c.id)) _channels.push_back(std::move(c));
        _gridTruncated = trimToGridCap(_channels);
    }

    [_table deselectAll:nil];
    [_table reloadData];
    [self setStatus:TrF(StringId::MacMainWindowChannelCountStatus,
                     {[NSString stringWithFormat:@"%lu", (unsigned long)_channels.size()],
                      q.empty() ? @"" : Tr(StringId::MacMainWindowSearchSuffix)})];
    [self updateEmptyHint];
}

// The "no channels yet" hint is a subview of pane 0, so it is only meaningful in Single view
// (in Split it would sit in the top-left quadrant; in PiP it hides behind the inset).
- (void)updateEmptyHint {
    // The hint says "no channels yet — click + Add Playlist", which is a lie on the 🎬 Movies
    // section head: the grid is empty there BY DESIGN (it prompts for a category) and the library
    // may hold tens of thousands of films. Suppress it for that row only.
    const BOOL onMoviesHead = (_filter.selectedItem.tag == kFilterMovies);
    _emptyHint.hidden = !_channels.empty() || _viewMode != ViewMode::Single || onMoviesHead;
}

// Settings ⚙ ▸ Channels ▸ Hide unavailable channels — toggle + persist, then re-filter the grid.
- (void)toggleHideDead:(id)__unused sender {
    _hideDead = !_hideDead;
    if (_db) _db->setSetting(L"hide_dead", _hideDead ? L"1" : L"0");
    [self refreshChannels];
}

// ---- actions ----

// "+ Add Playlist": prompt for a URL, then load it (peer of the Win32 onAddUrl).
- (void)addPlaylist:(id)__unused sender {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = Tr(StringId::AddPlaylistDialogTitle);
    a.informativeText = Tr(StringId::AddPlaylistUrlPrompt);
    [a addButtonWithTitle:Tr(StringId::ButtonOk)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    input.placeholderString = @"https://…";
    a.accessoryView = input;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;  // OK is the first button
        NSString* u = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (u.length) [self loadPlaylistFromURL:u];
    }];
}

// Download + parse + import a playlist URL off the main queue; newest request wins
// (an in-flight load is superseded, never interleaved).
- (void)loadPlaylistFromURL:(NSString*)u {
    const std::wstring url = ws(u);
    const uint64_t token = ++_loadToken;
    diag::addSecretsFromUrl(url);  // before anything logs it (platform/LogSecrets.h)
    [self setStatus:Tr(StringId::StatusDownloadingPlaylist)];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        std::string bytes;
        std::wstring derr;
        const bool ok = httpGet(url, bytes, derr);
        auto doc = std::make_shared<M3uDocument>(ok ? parseM3u(bytes) : M3uDocument{});
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != self->_loadToken) return;  // a newer load superseded this one
            if (!ok) { [self setStatus:TrF(StringId::MacMainWindowDownloadFailed, {ns(derr)})]; return; }
            [self importDoc:*doc name:friendlyName(url, true) source:url isUrl:true];
        });
    });
}

- (void)openFile:(id)__unused sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[ @"m3u", @"m3u8" ];
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSModalResponseOK || !panel.URL) return;
        const std::wstring path = ws(panel.URL.path);
        std::wstring perr;
        const M3uDocument doc = parseM3uFile(path, &perr);
        if (!perr.empty()) { [self setStatus:TrF(StringId::MacMainWindowReadFailed, {ns(perr)})]; return; }
        [self importDoc:doc name:friendlyName(path, false) source:path isUrl:false];
    }];
}

- (void)importDoc:(const M3uDocument&)doc
             name:(const std::wstring&)name
           source:(const std::wstring&)source
            isUrl:(bool)isUrl {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailableSavePlaylist)]; return; }
    const long long now = (long long)time(nullptr);
    // Pass doc.epgUrl (the M3U x-tvg-url) through so the playlist carries its XMLTV guide
    // source — without it, Refresh Guide has nothing to fetch (the Win32 peer at
    // MainWindowCommands.cpp:108 does the same).
    // The playlist's own guide URL (its x-tvg-url) can carry the login too — and for a playlist
    // imported from a FILE it is the only place we can learn it from. Register it now, not at the
    // next launch, so this session's stream lines are masked (platform/LogSecrets.h).
    diag::addSecretsFromUrl(doc.epgUrl);
    const long long pid = _db->addPlaylist(name, source, isUrl, now, doc.epgUrl);
    if (pid == 0) {
        [self setStatus:Tr(StringId::StatusAddPlaylistFailedDb)];
        [self showResults:Tr(StringId::PlaylistImportFailedHeading)
                     info:TrF(StringId::MacMainWindowImportDbSaveBody, {ns(source)})];
        return;
    }
    const int imported = _db->bulkInsertChannels(pid, doc.channels, now);
    const int parsed = (int)doc.channels.size();
    std::set<std::wstring> groups;
    for (const auto& c : doc.channels)
        if (!c.groupTitle.empty()) groups.insert(c.groupTitle);
    [self showPlaylist:pid];  // make it the active view (updates the grid + status)

    // Import results — the mac peer of the Win32 onPlaylistDone dialog.
    if (parsed == 0) {
        [self showResults:Tr(StringId::MacMainWindowPlaylistNoChannelsHeading)
                     info:TrF(StringId::MacMainWindowImportNoChannelsBody, {ns(source)})];
        return;
    }
    NSMutableString* info = [NSMutableString string];
    [info appendString:TrF(StringId::MacMainWindowImportSummaryBody,
        {ns(source), [NSString stringWithFormat:@"%d", parsed], [NSString stringWithFormat:@"%d", imported]})];
    if (parsed - imported > 0)
        [info appendString:TrF(StringId::MacMainWindowImportSkippedLine,
            {[NSString stringWithFormat:@"%d", parsed - imported]})];
    [info appendString:TrF(StringId::MacMainWindowImportGroupsLine,
        {[NSString stringWithFormat:@"%lu", (unsigned long)groups.size()]})];
    [self showResults:TrF(StringId::MacMainWindowImportedChannelsHeading,
        {[NSString stringWithFormat:@"%d", imported]}) info:info];
}

- (void)showResults:(NSString*)title info:(NSString*)info {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = info;
    [a addButtonWithTitle:Tr(StringId::ButtonOk)];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse __unused r) {}];
    [a autorelease];  // MRC: the sheet retains it for its lifetime; balance the alloc +1
}

- (void)filterChanged:(id)__unused sender { [self refreshChannels]; }

// 🔴 THE max_connections GATE. One connection is the whole budget on a real Xtream line, so a sync
// that runs during playback or a recording does not queue — it kicks the user's stream. Three
// terms, and each covers a case the others do not:
//   • every pane, because Split shows four;
//   • the SCHEDULED recorder, which is NOT in _panes — a scheduled recording runs with nothing
//     playing, and it is precisely what a sync would kick, after which the scheduler would mark
//     the truncated file Done. `_activeScheduleId != 0` is checked too because a start can be
//     in flight before isRecording() is true;
//   • detached players still stopping on a background queue — mac's peer of Win32's dyingPanes.
//     -teardownPane: and stopRecordingAsync both make isEngaged()/isRecording() read false while
//     the socket is still open.
// Keep this as ONE method so a future pane refactor cannot silently drop the scheduled recorder.
- (BOOL)isProviderConnectionBusy {
    if (_scheduleRecorder && _scheduleRecorder->isRecording()) return YES;
    if (_activeScheduleId != 0) return YES;                       // a start is in flight
    if (rabbitears::vlcDetachedPlayerCount() > 0) return YES;      // still letting go of a socket
    for (const auto& p : _panes)
        if (p->player && (p->player->isEngaged() || p->player->isRecording())) return YES;
    return NO;
}

// Sync the provider's movie catalogue. User-triggered (never a timer) precisely because of the
// gate above. The refusal ladder runs in Win32's order, and the module's running flag is claimed
// LAST, so no refusal here can wedge the menu item at "Syncing…".
- (void)syncMovies:(id)__unused sender {
    if (!_db) return;
    if (rabbitears::mac::vodSyncRunning()) return;                 // already going
    // The reverse of the interlock in -checkDeadLinks:. Both workers talk to the same provider, so
    // whichever starts second stands down rather than contending for the connection budget.
    if (rabbitears::mac::deadLinkSweepRunning()) {
        [self setStatus:Tr(StringId::StatusVodSyncPlaying)];
        return;
    }
    if ([self isProviderConnectionBusy]) {
        [self setStatus:Tr(StringId::StatusVodSyncPlaying)];
        return;
    }
    auto targets = rabbitears::mac::xtreamTargets(*_db);
    if (targets.empty()) {
        [self setStatus:Tr(StringId::StatusVodSyncNoProvider)];
        return;
    }

    rabbitears::mac::VodSyncRequest req;
    req.targets = std::move(targets);
    req.dbPath = rabbitears::Database::defaultDbPath();
    MainWindowController* __unsafe_unretained me = self;
    req.onProgress = [me](rabbitears::mac::VodSyncPhase ph, int n) {
        switch (ph) {
            case rabbitears::mac::VodSyncPhase::Contacting:
                [me setStatus:Tr(StringId::StatusVodSyncContacting)]; break;
            case rabbitears::mac::VodSyncPhase::Catchup:
                [me setStatus:Tr(StringId::StatusVodSyncFetchingCatchup)]; break;
            case rabbitears::mac::VodSyncPhase::Fetching:
                [me setStatus:Tr(StringId::StatusVodSyncFetching)]; break;
            case rabbitears::mac::VodSyncPhase::Saving:
                [me setStatus:TrF(StringId::StatusVodSyncSaving,
                                  {[NSString stringWithFormat:@"%d", n]})]; break;
        }
    };
    req.onDone = [me](rabbitears::mac::VodSyncReport rep) { [me vodSyncDidFinish:rep]; };
    // Catch-up flags written mid-sync: the grid's ↺ markers, the flags -playCatchup: checks and an
    // open TV Guide follow them now, not after the films (Win32 WM_APP_VOD_ARCHIVE).
    req.onArchive = [me]() { [me refreshArchiveMarkers]; };

    if (rabbitears::mac::startVodSync(std::move(req)) != rabbitears::mac::VodSyncStart::Started) return;
    _vodSyncRunning = YES;
    [self setStatus:Tr(StringId::StatusVodSyncContacting)];
}

// 🔴 DEFERRED OUT OF EVENT TRACKING, deliberately. The main queue drains in
// kCFRunLoopCommonModes, which includes NSEventTrackingRunLoopMode — so a dispatch_async
// completion runs WHILE a menu is tracking or a drag is in progress. Every row action here
// resolves _table.clickedRow at ACTION time (playClicked:, toggleFavourite:, editChannelNumber:,
// showInGuide:, playInPip:), and this completion replaces `_channels` wholesale, so applying it
// under an open menu makes the next click favourite or renumber a DIFFERENT film — silently, and
// persistently. An NSTimer scheduled in the DEFAULT run-loop mode cannot fire during tracking,
// which is exactly the property wanted: the work lands the moment the menu closes.
- (void)vodSyncDidFinish:(rabbitears::mac::VodSyncReport)rep {
    _vodSyncRunning = NO;
    _vodPendingReport = rep;
    [[NSRunLoop mainRunLoop] addTimer:[NSTimer timerWithTimeInterval:0
                                                             target:self
                                                           selector:@selector(applyVodSyncResult:)
                                                           userInfo:nil
                                                            repeats:NO]
                              forMode:NSDefaultRunLoopMode];
}

- (void)applyVodSyncResult:(NSTimer*)__unused t {
    using R = rabbitears::mac::VodSyncResult;
    const rabbitears::mac::VodSyncReport rep = _vodPendingReport;

    // Refresh keyed on COMMITTED work, not on the verdict: a run that inserted rows and then hit a
    // network error on a second line still changed the library, and the nav must show it.
    if (rep.inserted > 0 || rep.retired > 0) {
        [self rebuildFilterMenuPreservingSelection];
        // Land on 🎬 Movies when the sync actually added films and the user has not navigated
        // somewhere specific meanwhile. Staying on All Channels would re-materialise a capped
        // 5,000 rows of mostly live TV and latch the truncation notice — for an action whose
        // whole point was the films the user cannot see from there. Only ever moves the
        // selection FROM the default view, never away from a group/country/category the user
        // chose themselves (Win32 lands on Movies for the same reason).
        if (rep.inserted > 0 && _filter.selectedItem.tag == kFilterAll) {
            for (NSMenuItem* it in _filter.itemArray)
                if (it.tag == kFilterMovies) { [_filter selectItem:it]; break; }
        }
        [self refreshChannels];
    }

    NSString* msg = nil;
    switch (rep.result) {
        case R::Ok:
            msg = rep.retireRefused
                      ? TrF(StringId::StatusVodSyncDoneNoRetire,
                            {[NSString stringWithFormat:@"%d", rep.inserted],
                             [NSString stringWithFormat:@"%d", rep.retired],
                             [NSString stringWithFormat:@"%d", rep.unusable]})
                      : TrF(StringId::StatusVodSyncDone,
                            {[NSString stringWithFormat:@"%d", rep.inserted],
                             [NSString stringWithFormat:@"%d", rep.retired]});
            break;
        case R::EmptyCatalogue: msg = Tr(StringId::StatusVodSyncEmpty); break;
        case R::Cancelled:      msg = Tr(StringId::StatusVodSyncCancelled); break;
        default:
            // Partial vs outright failure: if rows were committed before it stopped, say so —
            // "failed" over a library that just gained 40,000 films is not a true report.
            msg = rep.inserted > 0
                      ? TrF(StringId::StatusVodSyncPartial,
                            {[NSString stringWithFormat:@"%d", rep.inserted], ns(rep.detail)})
                      : TrF(StringId::StatusVodSyncFailed, {ns(rep.detail)});
            break;
    }
    // Catch-up, after whatever the films said: how many channels now keep an archive, or why the
    // flags were left as they were. With two Xtream lines, one can succeed and the other fail: both.
    if (rep.archiveChannels >= 0)
        msg = [msg stringByAppendingString:TrF(StringId::StatusCatchupChannels,
                                               {[NSString stringWithFormat:@"%d", rep.archiveChannels]})];
    if (!rep.archiveDetail.empty())
        msg = [msg stringByAppendingString:TrF(StringId::StatusCatchupNotUpdated, {ns(rep.archiveDetail)})];
    [self setStatus:msg];
}

// Reset every channel to "not checked" — the undo for a wrong dead-status (Win32 parity).
// Confirmed, because it throws away results and there is no undo for the undo.
// Settings ▸ Logging ▸ <level>. Applies immediately (the threshold is a plain atomic, so the very
// next log line obeys it) and persists in the SHARED spelling, so a log_level written on Windows is
// understood here and vice versa.
- (void)selectLogLevel:(NSMenuItem*)sender {
    const diag::Level lv = static_cast<diag::Level>(sender.tag);
    diag::setLevel(lv);
    if (_db) _db->setSetting(L"log_level", wideFromUtf8(diag::levelToString(lv)));
    diag::info(L"log level set to " + wideFromUtf8(diag::levelToString(lv)));
}

// Reveal the log in Finder. The log sits next to the database, so it follows RABBITEARS_DATA_DIR —
// selecting the FILE rather than opening the folder means a tester lands on the right one even
// though the rotated rabbitears.log.1 is sitting beside it.
- (void)openLogFolder:(id)__unused sender {
    const std::filesystem::path db{rabbitears::Database::defaultDbPath()};
    const std::filesystem::path log = db.parent_path() / "rabbitears.log";
    NSString* p = [NSString stringWithUTF8String:log.string().c_str()];
    if (!p) return;
    if ([NSFileManager.defaultManager fileExistsAtPath:p]) {
        [NSWorkspace.sharedWorkspace selectFile:p inFileViewerRootedAtPath:@""];
    } else {
        // No log yet (or it was deleted) — open the folder itself rather than doing nothing.
        NSString* dir = [NSString stringWithUTF8String:db.parent_path().string().c_str()];
        if (dir) [NSWorkspace.sharedWorkspace selectFile:nil inFileViewerRootedAtPath:dir];
    }
}

// Start the background dead-link sweep. The REFUSAL LADDER runs here, in the controller, because
// only the controller knows the app state; DeadLinkSweep claims its running flag last.
//
// ⚠ THE PROVIDER-BUSY GATE IS A MAC ADDITION, and it is deliberate. Win32's startDeadLinkSweep
// refuses only while a VOD sync runs — but its own comment names the hazard this closes: "a probe
// refused for capacity is classified and can persist dead_status=Dead on a perfectly good live
// channel". On a max_connections:1 line, probing WHILE PLAYING is exactly that case, and it is
// worse here than for the sync: a sweep issues up to 250 connections, and the provider answers the
// ones over budget with 401/403 — which classifyProbe reads as an unambiguous refusal, i.e. Dead.
// The sweep would then mark the user's whole live list dead, and with "Hide unavailable" on, their
// library would vanish. sweepIsTrustworthy does NOT save us: those probes reached a server, so the
// batch looks perfectly healthy. Reuses the VOD sync's own gate and its wording. Flagged to Win32.
- (void)checkDeadLinks:(id)__unused sender {
    if (!_db) return;
    if (rabbitears::mac::deadLinkSweepRunning()) return;            // already going
    if (rabbitears::mac::vodSyncRunning()) {
        // The other worker that talks to the same provider. Contending is worse than either
        // waiting, for the misclassification reason above.
        [self setStatus:Tr(StringId::StatusVodSyncPlaying)];
        return;
    }
    if ([self isProviderConnectionBusy]) {
        [self setStatus:Tr(StringId::StatusVodSyncPlaying)];
        return;
    }

    rabbitears::mac::DeadLinkRequest req;
    req.dbPath = rabbitears::Database::defaultDbPath();
    MainWindowController* __unsafe_unretained me = self;
    req.onProgress = [me](int done, int total) {
        [me setStatus:TrF(StringId::StatusDeadLinkProgress,
                          {[NSString stringWithFormat:@"%d", done],
                           [NSString stringWithFormat:@"%d", total]})];
    };
    req.onDone = [me](rabbitears::mac::DeadLinkReport rep) { [me deadLinkSweepDidFinish:rep]; };

    if (rabbitears::mac::startDeadLinkSweep(std::move(req)) != rabbitears::mac::DeadLinkStart::Started)
        return;
    _sweepRunning = YES;
    [self setStatus:Tr(StringId::StatusDeadLinkStarted)];
}

// Delivered on the main queue by the worker.
//
// ⚠ IT MUST NOT REBUILD THE GRID HERE, and that is why this looks like -vodSyncDidFinish: rather
// than doing the obvious thing. dispatch_async(main) runs in EVERY run-loop mode, including
// NSEventTrackingRunLoopMode — so a completion landing while the user has the row context menu open
// (or is mid-drag) would re-query and re-order _channels UNDER the open menu. With "Hide
// unavailable" on, the newly-Dead rows drop out and everything below shifts up, so the menu's
// action then applies to whatever row slid into that index: favourite the wrong channel, renumber
// the wrong channel. A 250-probe sweep runs for minutes, so "the user is doing something else when
// it lands" is the NORMAL case, not a rare race. Deferring through an NSTimer in
// NSDefaultRunLoopMode makes the rebuild wait for tracking to end. (The VOD sync was restructured
// for this same hazard; an adversarial review caught this one repeating it.)
- (void)deadLinkSweepDidFinish:(rabbitears::mac::DeadLinkReport)rep {
    _sweepRunning = NO;
    _deadLinkPendingReport = rep;
    [[NSRunLoop mainRunLoop] addTimer:[NSTimer timerWithTimeInterval:0
                                                             target:self
                                                           selector:@selector(applyDeadLinkResult:)
                                                           userInfo:nil
                                                            repeats:NO]
                              forMode:NSDefaultRunLoopMode];
}

- (void)applyDeadLinkResult:(NSTimer*)__unused t {
    const rabbitears::mac::DeadLinkReport rep = _deadLinkPendingReport;
    if (rep.dbFailed) {
        // NOT the "clear failed" string: it names an operation the user did not run and a cause
        // (a busy database) that cannot be the one — the worker failed to OPEN its own connection.
        // "Inconclusive — nothing changed" is true for this case and points at the right place.
        [self setStatus:Tr(StringId::StatusDeadLinkInconclusive)];
        return;
    }
    if (!rep.trustworthy) {
        // The whole sweep was discarded. Say so plainly: nothing changed, and the likely cause is
        // this machine's connection, not the channels.
        [self setStatus:Tr(StringId::StatusDeadLinkInconclusive)];
        return;
    }
    if (rep.written > 0) [self refreshChannels];   // re-read: the worker wrote via its OWN handle
    [self setStatus:TrF(StringId::StatusDeadLinkDone,
                        {[NSString stringWithFormat:@"%d", rep.dead],
                         [NSString stringWithFormat:@"%d", rep.written]})];
}

- (void)clearDeadStatuses:(id)__unused sender {
    if (!_db) return;
    NSAlert* a = [[[NSAlert alloc] init] autorelease];
    a.messageText = Tr(StringId::MenuDeadLinkClear);
    // mac-only body: the SHARED DialogDeadLinkClearBody ends with "Run 'Check for dead links…'
    // again to re-test them", which is true on Windows (it ships the sweep) and false here — mac
    // has no sweep UI at all, and playback is the only thing that sets a dead status.
    a.informativeText = Tr(StringId::MacDialogDeadLinkClearBody);
    [a addButtonWithTitle:Tr(StringId::ButtonOk)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    MainWindowController* __unsafe_unretained me = self;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn || !me->_db) return;
        const int n = me->_db->clearDeadStatuses();
        if (n < 0) {  // the write FAILED — never claim success, the channels are still hidden
            [me setStatus:Tr(StringId::StatusDeadLinkClearFailed)];
            return;
        }
        // Rows hidden by "Hide unavailable" must come back NOW, not on the next navigation: the
        // grid filters on dead_status in SQL, and _channels is a cached vector.
        [me refreshChannels];
        [me setStatus:TrF(StringId::StatusDeadLinkCleared,
                          {[NSString stringWithFormat:@"%d", n]})];
    }];
}

// Live search, DEBOUNCED: a typing burst is one query, not one per keystroke. Even with the
// kMaxGridRows cap the search runs synchronously on the main thread, so on a large library each
// keystroke was a full query + table reload (Windows measured 1626 ms per keystroke before its
// cap). Re-arming a short one-shot timer collapses a burst into a single refresh after the user
// pauses. -refreshChannels cancels any pending shot itself, so a stale keystroke can never land
// on top of a filter change, a Categories apply, or a playlist reload.
- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object != _search) return;
    [_searchTimer invalidate];
    [_searchTimer autorelease];
    MainWindowController* __unsafe_unretained me = self;
    _searchTimer = [[NSTimer scheduledTimerWithTimeInterval:0.2 repeats:NO
                                                      block:^(NSTimer* __unused t) {
                                                          [me refreshChannels];
                                                      }] retain];
}

- (void)stop:(id)__unused sender {
    _player->stop();
    // Clear the pane's "now playing" so a later mode collapse can't resurrect the stream
    // the user explicitly stopped (carryStreamFromPane keys off channelId). Peer of the
    // Win32 Stop handler zeroing st->ap().nowPlayingId.
    if (MacVideoPane* p = [self activePane]) { p->channel = Channel{}; p->channelId = 0; }
    for (MeterView* m : _meters) [m reset];
    [self stopSpectrumTap];  // free the tap's RT thread (no spinning on silence)
    _window.title = Tr(StringId::AppName);
    [self setStatus:Tr(StringId::StatusStopped)];
    // NB: recording is independent of playback (its own connection) — Stop does NOT stop a
    // running recording, matching Win32. Use the Record button to stop recording.
}

// ---- recording (per-pane; the active pane's recorder) ------------------------------------
// The recorder is a second, headless libVLC player inside the pane's VlcPlayerMac. Recording
// is PER-PANE: the Record button toggles the ACTIVE pane's recorder and others keep recording.

// ~/Movies/RabbitEars, created on demand. (~/Movies is not TCC-protected, so no consent prompt;
// the app is hardened-runtime but not sandboxed, so no extra entitlement is needed to write it.)
- (NSString*)recordingsDir {
    NSArray<NSString*>* dirs =
        NSSearchPathForDirectoriesInDomains(NSMoviesDirectory, NSUserDomainMask, YES);
    NSString* base = dirs.firstObject ?: NSTemporaryDirectory();
    NSString* dir = [base stringByAppendingPathComponent:@"RabbitEars"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES
                                             attributes:nil error:nil];
    return dir;
}

// Map _recFormat to file extension + libVLC mux. Only ts and mp4 are offered on mac because
// the bundled VLC ships libmux_ts + libmux_mp4 but NO mkv muxer (libmkv_plugin is the DEMUXER),
// so mux=mkv would silently produce a broken file. mp4 writes its index on a clean stop (all our
// stops finalize) → playable; a hard crash mid-record loses it, unlike .ts (readable to the cut).
// Anything unrecognized (incl. a stale "mkv") falls back to ts, the crash-safest container.
+ (void)extForFormat:(const std::wstring&)fmt ext:(NSString**)ext mux:(std::string*)mux {
    if (fmt == L"mp4") { *ext = @".mp4"; *mux = "mp4"; }
    else               { *ext = @".ts";  *mux = "ts";  }
}

// A recording file path: <dir>/<sanitized channel> - <local timestamp><ext>. The channel name
// is scrubbed for BOTH filename safety and sout-MRL safety — `{ } ,` break the #std{…} chain
// and `/ :` etc. break the path, so the exact Win32 set is replaced with '_'.
- (NSString*)recordingPathFor:(NSString*)channelName ext:(NSString*)ext {
    NSMutableString* name = [NSMutableString string];
    // MRC: retain the cached set (app-lifetime). +characterSetWithCharactersInString: returns an
    // AUTORELEASED object; without the retain the static would dangle once the first call's
    // autorelease pool drained, and the NEXT call (e.g. a 2nd manual record, or the scheduler tick)
    // would message a freed object → EXC_BAD_ACCESS. (Caught on-device; shipped latent since 0.2.7.)
    static NSCharacterSet* bad =
        [[NSCharacterSet characterSetWithCharactersInString:@"\\/:*?\"<>|'{},"] retain];
    for (NSUInteger i = 0; i < channelName.length; ++i) {
        unichar c = [channelName characterAtIndex:i];
        [name appendString:(c < 0x20 || [bad characterIsMember:c]) ? @"_"
                                                                   : [NSString stringWithCharacters:&c length:1]];
    }
    if (name.length == 0) [name setString:@"channel"];
    NSDateFormatter* df = [[[NSDateFormatter alloc] init] autorelease];  // MRC: don't leak per record
    // en_US_POSIX so the fixed pattern yields a Gregorian, ASCII, 24-hour stamp regardless of the
    // user's regional calendar/locale (a non-POSIX locale could inject an era name, AM/PM, or a
    // non-Gregorian year into the filename).
    df.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    df.dateFormat = @"yyyy-MM-dd HH-mm-ss";
    NSString* stamp = [df stringFromDate:[NSDate date]];
    NSString* dir = [self recordingsDir];
    NSString* path = [dir stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"%@ - %@%@", name, stamp, ext]];
    // Uniquify: two recordings of the same channel within one second would collide (same stamp);
    // append " (N)" so the second never overwrites the first's file.
    for (int n = 2; [NSFileManager.defaultManager fileExistsAtPath:path] && n < 1000; ++n)
        path = [dir stringByAppendingPathComponent:
                [NSString stringWithFormat:@"%@ - %@ (%d)%@", name, stamp, n, ext]];
    return path;
}

- (void)toggleRecord:(id)__unused sender {
    MacVideoPane* p = [self activePane];
    if (!p) return;
    // Recording opens a SECOND connection to the provider (the headless recorder has its own
    // socket), so it claims connection budget exactly like playback does — stand the sync down.
    // Harmless on the stop branch below. Win32 gates the same handler (onToggleRecord).
    rabbitears::mac::cancelVodSync();
    // Same rule, same reason: the sweep is the OTHER provider-facing worker, and its probes are
    // worse to leave running than the sync's downloads — a probe refused for connection capacity
    // reads as an unambiguous rejection and persists dead_status=Dead on a working channel.
    rabbitears::mac::cancelDeadLinkSweep();
    if (p->player->isRecording()) {
        const std::wstring file = p->player->recordingFile();
        p->player->stopRecordingAsync();  // off-main: a stalled feed can't hang the UI on stop
        [self updateRecordButton];
        [self syncKeepAwake];  // another pane may still record — re-derive, don't assume
        [self setStatus:TrF(StringId::StatusRecordingSaved, {ns(file)})];
        return;
    }
    if (p->catchup) {  // a programme from the archive, not a channel: nothing to record live
        [self setStatus:Tr(StringId::StatusCatchupNoRecord)];
        return;
    }
    if (p->channelId == 0) { [self setStatus:Tr(StringId::StatusPlayChannelFirst)]; return; }
    NSString* ext; std::string mux;
    [MainWindowController extForFormat:_recFormat ext:&ext mux:&mux];
    NSString* path = [self recordingPathFor:ns(p->channel.name) ext:ext];
    if (p->player->startRecording(p->channel.streamUrl, p->channel.userAgent, p->channel.referrer,
                                  ws(path), mux)) {
        [self updateRecordButton];
        [self syncKeepAwake];  // don't let the machine idle-sleep out from under a recording
        [self setStatus:TrF(StringId::StatusRecordingNow, {ns(p->channel.name), path})];
    } else {
        [self setStatus:Tr(StringId::MacMainWindowRecordingStartFailed)];
    }
}

- (void)finalizeRecordingsForQuit {
    // Synchronously finalize recordings still attached to a live pane — the common case (a
    // recording the user left running). A recorder already detached to a background stop (a
    // just-clicked async Stop, or a pane collapsed via -teardownPane:) races process exit; for
    // the default .ts that's harmless (readable to the cut), and for mp4 it's a narrow window
    // (quit within ~1s of that stop) where the index may not be written. Draining those isn't
    // worth a global in-flight tracker here.
    for (auto& pane : _panes)
        if (pane->player) pane->player->stopRecording();  // synchronous flush + index write
    if (_scheduleRecorder) _scheduleRecorder->stopRecording();  // finalize a scheduled recording too
    g_epgCancel = true;  // a running guide refresh stops at its next transaction boundary
    if (_keepAwake != 0) { IOPMAssertionRelease(_keepAwake); _keepAwake = 0; }
    [_schedulerTimer invalidate]; _schedulerTimer = nil;
    // Last chance for a schedule decision whose write was lost: the overlay that protected it lives
    // in memory, so a "cancelled" airing that never reached the DB would otherwise still be Pending
    // on the next launch. The workers that contend for the lock were cancelled by the recording
    // stops above only in the sense that nothing new starts — so this is BEST-EFFORT (Win32 parity).
    if (_statusWriter) _statusWriter->flushAtQuit();
}

// ---- recording scheduler (Phase 5/6) ----------------------------------------------------
// A ~30s tick applies the shared planScheduler() core to the queue and expands EPG rules. A
// DEDICATED headless recorder runs scheduled recordings so they never fight the visible panes'
// manual recorders. NB: this only records while the app is RUNNING and the Mac is awake — a
// non-root app cannot arm a system wake (see -showRecordings: copy and mac/HANDOVER.md).

- (void)startScheduler {
    if (!_db || !_engine) return;
    _scheduleRecorder = std::make_unique<VlcPlayerMac>();
    _scheduleRecorder->init(*_engine);  // headless: only ever records, never attaches a view
    // Weak-self timer (app-lifetime controller, but be correct): the tick fires ~every 30s.
    MainWindowController* __unsafe_unretained me = self;
    _schedulerTimer = [NSTimer scheduledTimerWithTimeInterval:30.0 repeats:YES
                                                       block:^(NSTimer* __unused t) { [me schedulerTick]; }];
    [self schedulerTick];  // run once now so an already-due schedule fires promptly
}

- (void)stopScheduledRecorder {
    if (_scheduleRecorder) _scheduleRecorder->stopRecordingAsync();  // off-main flush
    _activeScheduleId = 0;
}

// The Recordings window cancelled schedule `sid`: stop the recorder if that is the one it is on.
- (void)stopScheduleIfActive:(long long)sid {
    if (sid != 0 && sid == _activeScheduleId) {
        diag::info(L"scheduled recording stopped by the user (id " + std::to_wstring(sid) + L")");
        const bool fromRule = _activeScheduleRow.ruleId != 0;
        const std::wstring file = _scheduleRecorder ? _scheduleRecorder->recordingFile() : std::wstring();
        [self stopScheduledRecorder];
        // Replace "● Recording (scheduled) …", which would otherwise linger: Win32's skip wording for
        // a series airing (the rule keeps going), else where the part already recorded went.
        if (fromRule) [self setStatus:Tr(StringId::StatusAiringSkippedRule)];
        else if (!file.empty()) [self setStatus:TrF(StringId::StatusRecordingSaved, {ns(file)})];
    }
}

// A schedule row by id in a listing (nullptr when the listing does not have it).
static const rabbitears::ScheduledRecording* findScheduleRow(
    const std::vector<rabbitears::ScheduledRecording>& rows, long long id) {
    for (const auto& r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

- (void)schedulerTick {
    if (!_db || !_statusWriter) return;
    using rabbitears::ScheduleStatus;
    // Land any terminal status an earlier write lost BEFORE listing, then list with whatever is
    // still un-landed overlaid, so the planner acts on what the app decided (ScheduleStatusWriter).
    const bool flushLost = _statusWriter->flush();
    auto schedules = _statusWriter->listAsDecided();

    // Startup reconcile: a row still marked Recording on the FIRST tick is stale (a prior session
    // closed mid-record; nothing is actually recording now). Reset it to Pending so planScheduler
    // resumes it if still in-window, or misses it. (Peer of Win32's schedulerReconciled.)
    //
    // The stale set is captured once, and each id leaves it only when its reset LANDS (or the row
    // stops reading Recording). This used to latch after one attempt: a reset lost to contention
    // left the row at Recording — and planScheduler counts any Recording row as "recorder busy" —
    // so it blocked EVERY schedule until its stop time, when plan.stop closed it out as a false Done.
    if (!_schedulerReconciled) {
        _schedulerReconciled = YES;
        for (const auto& s : schedules)
            if (s.status == ScheduleStatus::Recording) _staleRecordingIds.push_back(s.id);
    }
    if (!_staleRecordingIds.empty()) {
        bool changed = false;
        // One writer lock: after the first loss (including one the flush just took) don't stall on
        // the rest; a skipped reset retries next tick.
        bool contended = flushLost;
        bool loggedStuck = false;  // once per tick — the only sign that no schedule can start
        for (auto it = _staleRecordingIds.begin(); it != _staleRecordingIds.end();) {
            const rabbitears::ScheduledRecording* row = findScheduleRow(schedules, *it);
            // Missing from THIS listing: kept — a failed read looks exactly like "gone".
            if (!row) { ++it; continue; }
            // The active schedule is the one row a reset must never touch (it would restart a live
            // recording); a row no longer Recording has nothing left to reconcile.
            if (row->status != ScheduleStatus::Recording || *it == _activeScheduleId) {
                it = _staleRecordingIds.erase(it);
            } else if (!contended && _db->updateScheduleStatus(*it, ScheduleStatus::Pending)) {
                changed = true;
                it = _staleRecordingIds.erase(it);
            } else {
                if (!loggedStuck) {
                    diag::error(L"stale Recording row could not be reset (id " + std::to_wstring(*it) +
                                L") — database busy; retrying next tick (no schedule can start while "
                                L"this repeats — until it lands, or this row's stop time closes it out)");
                    loggedStuck = true;
                }
                contended = true;
                ++it;
            }
        }
        if (changed) schedules = _statusWriter->listAsDecided();
    }

    // A recording whose stream DIED before its stop time (refused, a 404, the provider dropped the
    // connection). libVLC moves the recorder to Error/Ended, but the player object stays, so nothing
    // else notices: the row sat at "● Recording" over a file that had stopped growing, planScheduler
    // counted the recorder busy (every other schedule blocked), the VOD-sync gate refused ("stop
    // recording first") with no way to stop it, and at the stop time plan.stop called it Done.
    // Found on device, 2026-11 (a 404 behind a live airing). Close it out honestly instead: stop
    // the recorder (synchronously — its input is already over, so there is nothing to stall on, and
    // the file is final before we look at it), drop the file if nothing was ever written (a 0-byte
    // .ts is only litter), keep it if it has data (the part that did record), and mark the row
    // Failed — through the writer, so a lost write is remembered rather than leaving a Recording row
    // with nothing behind it. No retry: hammering a refusing provider every tick is worse.
    if (_activeScheduleId != 0 && _scheduleRecorder) {
        using RS = VlcPlayerMac::RecorderState;
        const RS rs = _scheduleRecorder->recorderState();
        if (rs == RS::Ended || rs == RS::Failed) {
            const long long id = _activeScheduleId;
            const std::wstring file = _scheduleRecorder->recordingFile();
            _scheduleRecorder->stopRecording();
            _activeScheduleId = 0;
            NSString* path = file.empty() ? nil : ns(file);
            const unsigned long long bytes =
                path ? [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize] : 0;
            if (path && bytes == 0) [NSFileManager.defaultManager removeItemAtPath:path error:nil];
            rabbitears::ScheduledRecording fallback = _activeScheduleRow;  // when the listing misses it
            fallback.status = ScheduleStatus::Recording;  // beginScheduledStart persisted this first
            const rabbitears::ScheduledRecording* row = findScheduleRow(schedules, id);
            _statusWriter->setTerminal(id, row ? row : &fallback, ScheduleStatus::Failed);
            diag::warn(L"scheduled recording FAILED mid-airing (id " + std::to_wstring(id) + L"): its stream " +
                       (rs == RS::Failed ? L"errored" : L"ended") + L" before the stop time; " +
                       (bytes ? L"kept " + std::to_wstring(bytes) + L" bytes" : L"nothing was written, empty file removed"));
            [self setStatus:TrF(StringId::StatusScheduledRecordingFailed, {ns(fallback.channelName)})];
            schedules = _statusWriter->listAsDecided();
        }
    }

    const long long now = (long long)time(nullptr);
    // The dedicated recorder is used ONLY for schedules, so no "manual recording" blocks it.
    const rabbitears::SchedulerPlan plan = rabbitears::planScheduler(schedules, now, /*manual=*/false);
    // The planner's verdict at DEBUG, only when it decided something: a missed recording is the
    // hardest thing to diagnose after the fact, and an idle tick logs nothing.
    if (!plan.start.empty() || !plan.stop.empty() || !plan.miss.empty())
        diag::debug(L"scheduler: " + std::to_wstring(schedules.size()) + L" queued, start=" +
                    std::to_wstring(plan.start.size()) + L" stop=" + std::to_wstring(plan.stop.size()) +
                    L" miss=" + std::to_wstring(plan.miss.size()));

    for (long long id : plan.stop) {
        if (id == _activeScheduleId) [self stopScheduledRecorder];
        // Remembered if lost: a row left at Recording would block every other schedule.
        _statusWriter->setTerminal(id, findScheduleRow(schedules, id), ScheduleStatus::Done);
        diag::info(L"scheduled recording finished (id " + std::to_wstring(id) + L")");
        [self setStatus:Tr(StringId::StatusScheduledRecordingSaved)];
    }
    for (long long id : plan.miss) {
        _statusWriter->setTerminal(id, findScheduleRow(schedules, id), ScheduleStatus::Missed);
        diag::warn(L"scheduled recording missed (id " + std::to_wstring(id) + L")");
    }
    for (long long id : plan.start) {  // planScheduler yields at most one
        const rabbitears::ScheduledRecording* s = nullptr;
        for (const auto& x : schedules) if (x.id == id) { s = &x; break; }
        if (!s) continue;
        // 🔴 A DUE RECORDING OUTRANKS A CATALOGUE REFRESH — cancel the sync and carry on, never
        // defer the recording. The recording is time-critical user data that cannot be re-made;
        // a movie sync can be re-run at any time.
        //
        // This deliberately does NOT stand the scheduler down. An earlier revision did, to avoid
        // contending with the worker's write transaction, and that was a worse bug: the deferral
        // had no bound, so a slow or multi-target sync could push a recording past its whole
        // window, at which point plan.miss marks it Missed — silently losing the recording it was
        // trying to protect. Win32 has always done it this way round (onToggleRecord and
        // onSchedulerTick both call cancelVodSync; Win32/HANDOVER.md:573).
        //
        // Cancellation is soft — an in-flight HTTP body finishes — but no further request is
        // issued and, critically, no further DB write is attempted, so the writer lock is released
        // at the current transaction boundary rather than held for the rest of the catalogue.
        rabbitears::mac::cancelVodSync();
    // Same rule, same reason: the sweep is the OTHER provider-facing worker, and its probes are
    // worse to leave running than the sync's downloads — a probe refused for connection capacity
    // reads as an unambiguous rejection and persists dead_status=Dead on a working channel.
    rabbitears::mac::cancelDeadLinkSweep();
        NSString* ext; std::string mux;
        [MainWindowController extForFormat:s->mux ext:&ext mux:&mux];
        NSString* path = [self recordingPathFor:ns(s->channelName) ext:ext];
        // Persist Recording FIRST and start only if it landed — beginScheduledStart (common/core/
        // RecordingScheduler.h) holds the why. The old order (start, THEN write) turned one lost
        // write into a truncated recording: the row stayed Pending, so the next tick re-started it
        // over the file in progress, leaving a fragment each time. (Flagged by this team in
        // 2026-08; fixed in the shared core by Windows in b4b3e4c.)
        const rabbitears::ScheduledStart outcome = rabbitears::beginScheduledStart(
            [&] { return _db->updateScheduleStatus(id, ScheduleStatus::Recording, ws(path)); },
            [&] {
                return _scheduleRecorder &&
                       _scheduleRecorder->startRecording(s->streamUrl, s->userAgent, s->referrer, ws(path), mux);
            });
        switch (outcome) {
            case rabbitears::ScheduledStart::Started:
                _activeScheduleId = id;
                _activeScheduleRow = *s;  // identity for a later terminal decision about it
                diag::info(L"scheduled recording started: " + s->channelName + L" -> " + ws(path));
                [self setStatus:TrF(StringId::StatusRecordingScheduledNow, {ns(s->channelName)})];
                break;
            case rabbitears::ScheduledStart::NotPersisted:
                // Nothing started, nothing owned; the row is still Pending, so the next tick's plan
                // re-emits it — each lost attempt delays the start by a tick (~30 s).
                diag::error(L"scheduled recording NOT started: its Recording status write was lost "
                            L"(database busy) — will retry next tick (id " + std::to_wstring(id) + L")");
                break;
            case rabbitears::ScheduledStart::RecorderFailed:
                // The row already says Recording, so Failed must land (or be remembered), or it
                // would read as busy with nothing recording.
                _statusWriter->setTerminal(id, s, ScheduleStatus::Failed);
                diag::error(L"scheduled recording failed to start (id " + std::to_wstring(id) + L")");
                break;
        }
    }

    [self expandRecordingRules:NO];  // Phase 6: queue upcoming airings from series rules
    [self syncKeepAwake];
    if (_recordingsWC) [(id)_recordingsWC reload];  // refresh an open Recordings window
}

// Phase 6: expand EPG series rules into concrete scheduled_recordings rows. The shared
// expandRules() does the matching/dedup/padding; the controller resolves each channel and
// inserts. Heavy (14-day EPG across every playlist), so throttled unless `force` (a guide
// refresh, a new/re-enabled rule). Returns the number of airings queued.
- (int)expandRecordingRules:(BOOL)force {
    if (!_db) return 0;
    const auto rules = _db->listRules();
    if (rules.empty()) return 0;  // common case: no cost
    const long long now = (long long)time(nullptr);
    if (!force && _rulesExpandedAt != 0 &&
        now - _rulesExpandedAt < rabbitears::kRuleExpandIntervalSeconds)
        return 0;
    const long long horizon = now + rabbitears::kRuleHorizonSeconds;
    const auto programmes = _db->programmesInWindowAll(now, horizon);
    if (programmes.empty()) { _rulesExpandedAt = now; return 0; }

    // Index the library by NORMALISED tvg-id (the @feed quality suffix stripped + case-folded) so an
    // iptv-org "@feed" channel still matches the XMLTV base id — the exact channelByTvgId would miss
    // it and the rule would silently record nothing. Built once; it doubles as the recordable set
    // for the pre-filter below, and the resolver for the planned schedules after.
    std::map<std::wstring, rabbitears::Channel> byNorm;
    for (const auto& c : _db->allChannels())
        if (!c.tvgId.empty()) byNorm.emplace(rabbitears::normaliseTvgId(c.tvgId), c);

    // Episode dedup lives inside the channel-blind expander, so it would otherwise claim an episode
    // for an EPG-only channel we can't record (a shared XMLTV feed routinely carries channels no
    // enabled playlist has), silently dropping the recordable airing of that same episode. Keep only
    // programmes on a recordable (library) channel so dedup weighs only airings we can actually
    // record. (Win32 0.2.9 parity — matched on the normalised id here for @feed robustness.)
    std::vector<rabbitears::Programme> recordableProgs;
    recordableProgs.reserve(programmes.size());
    for (const auto& p : programmes)
        if (byNorm.count(rabbitears::normaliseTvgId(p.channelId))) recordableProgs.push_back(p);
    if (recordableProgs.empty()) { _rulesExpandedAt = now; return 0; }

    // expandRules stamps each schedule's channelId with the EPG programme's base id; resolve it back
    // to the library channel through the same normalised index.
    const auto planned =
        rabbitears::expandRules(rules, recordableProgs, _db->listSchedules(), now, horizon);
    int added = 0;
    for (rabbitears::ScheduledRecording s : planned) {
        auto it = byNorm.find(rabbitears::normaliseTvgId(s.channelId));
        if (it == byNorm.end()) continue;  // channel left the library → nothing recordable
        const rabbitears::Channel& ch = it->second;
        if (!ch.name.empty()) s.channelName = ch.name;
        s.streamUrl = ch.streamUrl;
        s.userAgent = ch.userAgent;
        s.referrer  = ch.referrer;
        s.createdAt = now;
        if (_db->addSchedule(s) > 0) ++added;
    }
    _rulesExpandedAt = now;
    return added;
}

// Phase 5: queue a self-contained one-off recording for a specific channel + time window
// (from the TV Guide's programme dialog). Resolves the tvg-id to a stream now, so the
// recording survives the channel later changing/leaving. Nudges the scheduler in case it is
// already airing.
- (void)scheduleRecordingForTvgId:(NSString*)tvgId
                             name:(NSString*)name
                            title:(NSString*)title
                            start:(long long)startUtc
                             stop:(long long)stopUtc {
    if (!_db) return;
    auto ch = tvgId.length ? _db->channelByTvgId(ws(tvgId)) : std::nullopt;
    if (!ch) {
        [self showResults:Tr(StringId::MacMainWindowCantScheduleHeading)
                     info:TrF(StringId::MacMainWindowScheduleNoMatchBody, {name})];
        return;
    }
    rabbitears::ScheduledRecording s;
    s.channelId   = ws(tvgId);
    s.channelName = ch->name.empty() ? ws(name) : ch->name;
    s.streamUrl   = ch->streamUrl;
    s.userAgent   = ch->userAgent;
    s.referrer    = ch->referrer;
    s.title       = ws(title);
    s.startUtc    = startUtc;
    s.stopUtc     = stopUtc;
    s.mux         = _recFormat;
    s.createdAt   = (long long)time(nullptr);
    if (_db->addSchedule(s) > 0) {
        [self schedulerTick];  // fire immediately if it's already in-window
        [self setStatus:TrF(StringId::MacMainWindowScheduledStatus,
                         {ns(s.channelName),
                          title.length ? title : Tr(StringId::MacMainWindowScheduledFallbackTitle)})];
        // Phase 7 — the honest wake caveat, shown once at schedule time. A non-root app can't
        // wake a sleeping Mac (IOPMSchedulePowerEvent needs root), so a scheduled recording only
        // fires if RabbitEars is running and the Mac is awake when its window opens.
        if (startUtc > (long long)time(nullptr))
            [self showResults:TrF(StringId::MacMainWindowScheduledTitle, {ns(s.channelName)})
                         info:Tr(StringId::MacMainWindowScheduledWakeCaveat)];
    } else {
        [self showResults:Tr(StringId::MacMainWindowCantScheduleHeading)
                     info:Tr(StringId::MacMainWindowScheduleSaveFailedBody)];
    }
}

// Phase 6: create a series rule that records every future airing matching a programme's title
// on its channel (from the guide's "Record Series"). Forces an immediate expansion.
- (void)addSeriesRuleForTvgId:(NSString*)tvgId name:(NSString*)name title:(NSString*)title {
    if (!_db || title.length == 0) return;
    rabbitears::RecordingRule r;
    r.channelId   = ws(tvgId);
    r.channelName = ws(name);
    r.titleMatch  = ws(title);
    r.match       = rabbitears::RuleMatch::Exact;
    r.enabled     = true;
    r.mux         = _recFormat;
    r.createdAt   = (long long)time(nullptr);
    if (_db->addRule(r) > 0) {
        const int queued = [self expandRecordingRules:YES];
        [self schedulerTick];
        [self setStatus:TrF(StringId::MacMainWindowSeriesRuleAdded,
                         {title, [NSString stringWithFormat:@"%d", queued]})];
    } else {
        [self showResults:Tr(StringId::MacMainWindowCantAddRuleHeading)
                     info:Tr(StringId::MacMainWindowSeriesRuleSaveFailedBody)];
    }
}

// The button reflects the ACTIVE pane's recording state, so it stays correct as the user
// switches panes (call from -setActivePane:). Red filled dot = recording.
- (void)updateRecordButton {
    MacVideoPane* p = [self activePane];
    const BOOL rec = p && p->player->isRecording();
    NSString* sym = rec ? @"stop.circle.fill" : @"record.circle";
    _recBtn.image = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:Tr(StringId::TooltipBtnRecord)];
    _recBtn.contentTintColor = rec ? NSColor.systemRedColor : nil;
    _recBtn.toolTip = Tr(rec ? StringId::MacMainWindowStopRecordingPaneTooltip
                             : StringId::MacMainWindowRecordActivePaneTooltip);
}

// Hold an IOPMAssertion iff ANY pane is recording, so the Mac won't IDLE-sleep mid-recording.
// (This does NOT wake a sleeping Mac and does not survive a lid close / low battery / Dark
// Wake — macOS gives a non-root, hardened app no way to arm a wake. See mac/HANDOVER.md.)
- (void)syncKeepAwake {
    BOOL anyRecording = _scheduleRecorder && _scheduleRecorder->isRecording();
    for (auto& pane : _panes)
        if (pane->player && pane->player->isRecording()) { anyRecording = YES; break; }
    if (anyRecording && _keepAwake == 0) {
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                    kIOPMAssertionLevelOn, CFSTR("RabbitEars recording"),
                                    &_keepAwake);
    } else if (!anyRecording && _keepAwake != 0) {
        IOPMAssertionRelease(_keepAwake);
        _keepAwake = 0;
    }
}

// 250ms libVLC-stats poll → the stream-health meter. No audio capture (no consent,
// no A/V desync): the bar tracks throughput against a slowly-decaying rolling peak,
// so it fills while data flows at its usual rate and drops when the stream stalls.
- (void)tickStats {
    // Re-assert the single-audio model on EVERY pane each tick (both directions — setMuted is
    // idempotent). Muting the background survives a libVLC aout recreation on a quality switch;
    // re-asserting the ACTIVE pane's unmute is what heals a pane whose audio ES wasn't ready
    // (or whose track ids changed across an ad break) when it was first activated — otherwise
    // that pane stays silent forever.
    if (_panes.size() > 1)
        for (int i = 0; i < (int)_panes.size(); ++i)
            _panes[(size_t)i]->player->setMuted(i != _activePane);

    // "Hide unavailable channels" heuristic: judge ONLY the active pane's channel (the one being
    // watched). libVLC Playing -> mark it Alive; Error (failed to open) -> Dead; other states leave
    // the mark unchanged. Marked at most once per change (ap->channel.deadStatus tracks the last
    // mark), so a background/never-played channel is never touched and a working channel can't be
    // flagged dead by a pane the user isn't watching.
    if (MacVideoPane* ap = [self activePane]; ap && ap->channelId && _db) {
        DeadStatus want = DeadStatus::Unknown;
        switch (ap->player->playState()) {
            case VlcPlayerMac::PlayState::Playing: want = DeadStatus::Alive; ap->everPlayed = true; break;
            case VlcPlayerMac::PlayState::Error:   want = DeadStatus::Dead;  break;
            default: break;
        }
        // Only DEMOTE to Dead on a genuine OPEN failure (never reached Playing this attempt). libvlc_Error
        // is ANY fatal error incl. mid-playback, and it is TERMINAL (never recovers on its own), so a blip
        // on a channel that WAS live must not latch it Dead+hidden with no way back.
        const bool demoteBlocked = (want == DeadStatus::Dead && ap->everPlayed);
        if (want != DeadStatus::Unknown && want != ap->channel.deadStatus && !demoteBlocked) {
            ap->channel.deadStatus = want;
            _db->setDeadStatus(ap->channelId, want, (long long)time(nullptr));
            // Reflect it in the visible list WITHOUT the heavy -refreshChannels (which deselects + resets
            // the status line): update the row in place, dropping it only if it is now Dead and Hide is on.
            for (auto it = _channels.begin(); it != _channels.end(); ++it) {
                if (it->id != ap->channelId) continue;
                if (want == DeadStatus::Dead && _hideDead) _channels.erase(it);
                else it->deadStatus = want;
                break;
            }
            [_table reloadData];  // re-grey / drop; preserves the selection (unlike -refreshChannels)
        }
    }

    // Catch-up: an archive with nothing for that time fails BEFORE it ever plays — say so plainly,
    // once, instead of the generic silence (Win32 StatusCatchupNotInArchive).
    if (MacVideoPane* ap = [self activePane]; ap && ap->catchup) {
        const auto ps = ap->player->playState();
        if (ps == VlcPlayerMac::PlayState::Playing) {
            ap->catchupPlayed = true;
        } else if (!ap->catchupPlayed && (ps == VlcPlayerMac::PlayState::Error ||
                                          ps == VlcPlayerMac::PlayState::Ended)) {
            ap->catchupPlayed = true;  // said once
            [self setStatus:TrF(StringId::StatusCatchupNotInArchive, {ns(ap->channel.name)})];
        }
    }

    // ⚠ TRANSPORT FIRST — ABOVE the early return below, and deliberately so. mac attaches NO
    // libVLC event callbacks (see the TODO at the top of VlcPlayerMac.mm), so this tick is the
    // ONLY thing in the process that can ever retire the scrub bar. Below the `!fs.playing`
    // return, a film reaching its end (libvlc_Ended: not playing, no user action, no callback)
    // would strand the bar on screen forever with no way back. Stop has an explicit reset site;
    // end-of-film has none. Win32 is immune because it drives its seek UI off every player
    // event; mac gets no second chance.
    [self updateTransport];

    const FlowStats fs = _player->sampleStats();
    // The data-flow tank (Win32 MainWindow: Opening/Buffering/Playing events + the Stats tick, here polled):
    // health from the active pane's player state, flow = throughput through a soft knee (~768 kb/s), trouble
    // = corruption + discontinuities + half the dropped frames, and the Mb/s readout. Fed before the early
    // return below, so a stopped stream drains the tank.
    if (_tank && !_tank.hidden) {
        if (MacVideoPane* ap = [self activePane]) [_tank setHealth:ap->player->bufferHealth()];
        const float flow = fs.playing ? (float)(1.0 - std::exp(-fs.demuxBytesPerSec / 96000.0)) : 0.0f;
        const float tankTrouble = std::clamp(
            (fs.corruptedDelta + fs.discontinuityDelta + 0.5f * fs.lostPicturesDelta) / 6.0f, 0.0f, 1.0f);
        [_tank setFlow:flow trouble:tankTrouble];
        NSString* m = @"";
        if (fs.playing && fs.demuxBytesPerSec > 0.0) {
            const double bps = fs.demuxBytesPerSec * 8.0;
            m = bps >= 1.0e6 ? [NSString stringWithFormat:Tr(StringId::MeterOverlayMbps), bps / 1.0e6]
                             : [NSString stringWithFormat:Tr(StringId::MeterOverlayKbps), bps / 1.0e3];
        }
        [_tank setMetrics:m];
    }
    // NOTE: do NOT reset _spectrumSilentTicks here. libVLC's is_playing() dips false at HLS
    // segment boundaries, and zeroing the denial counter on every dip meant it could never
    // accumulate its window on a real stream (see -updateSpectrumAvailability:).
    if (!fs.playing) { for (MeterView* m : _meters) [m reset]; return; }
    // Bitrate: the DEMUX byte-rate — HLS/segmented inputs report i_read_bytes as 0, but
    // the demux rate tracks the real media bitrate for both HLS and plain streams.
    [_meters[(int)MeterKind::Bitrate] pushBitrate:fs.demuxBytesPerSec];
    [_meters[(int)MeterKind::Frames] setFrames:(int)std::lround(fs.displayedPerSec)
                                    dropsDelta:fs.lostPicturesDelta];
    // Signal: a composite of stream health — errors (corrupt/discontinuity/lost) raise
    // "trouble" (red tint) and cut strength; a clean, flowing stream reads near-full.
    const float trouble = std::clamp(
        (float)(fs.corruptedDelta + fs.discontinuityDelta + fs.lostPicturesDelta) / 5.0f, 0.0f, 1.0f);
    [_meters[(int)MeterKind::Signal] setSignal:std::clamp(1.0f - trouble * 0.7f, 0.15f, 1.0f)
                                       trouble:trouble];
    [self updateSpectrumAvailability:fs];
}

// ---- Meters (per-kind config from the dialog + persistence) ------------------
// Each MeterView is shown/positioned per its MeterConfig. Only Spectrum needs the
// audio-capture tap (opt-in); Signal/Bitrate/Frames run off FlowStats.

// Load per-kind enable/style/colours/tuning from the settings DB (Win32-compatible keys
// meter_<kind> / _style / _colors / _tuning); defaults otherwise.
- (void)loadMeterConfig {
    static const char* keys[4] = {"spectrum", "signal", "bitrate", "frames"};
    static const MeterKind kinds[4] = {MeterKind::Spectrum, MeterKind::Signal,
                                       MeterKind::Bitrate, MeterKind::Frames};
    for (int k = 0; k < 4; ++k) {
        _meterCfg[k] = MeterConfig{};
        _meterCfg[k].enabled = false;
        _meterCfg[k].style   = defaultMeterStyle(kinds[k]);
        _meterCfg[k].palette = defaultMeterPalette(kinds[k]);
        _meterCfg[k].tuning  = defaultMeterTuning();
        if (!_db) continue;
        const std::wstring base = wideFromUtf8((std::string("meter_") + keys[k]).c_str());
        if (auto e = _db->getSetting(base)) _meterCfg[k].enabled = (*e == L"1");
        if (auto s = _db->getSetting(base + L"_style"))
            _meterCfg[k].style = meterStyleFromString(utf8FromWide(*s), _meterCfg[k].style);
        if (auto c = _db->getSetting(base + L"_colors"))
            _meterCfg[k].palette = meterPaletteFromString(utf8FromWide(*c), _meterCfg[k].palette);
        if (auto t = _db->getSetting(base + L"_tuning"))
            _meterCfg[k].tuning = meterTuningFromString(utf8FromWide(*t), _meterCfg[k].tuning);
    }
    if (_db) {
        if (auto h = _db->getSetting(L"meters_hidden")) _metersHidden = (*h == L"1");
        _meterHeightDp = rabbitears::mac::kMeterHeightStd;
        if (auto mh = _db->getSetting(L"meter_height"))
            _meterHeightDp = rabbitears::mac::clampMeterHeightDp((int)std::wcstol(mh->c_str(), nullptr, 10));
        if (auto ml = _db->getSetting(L"meter_labels")) _meterLabels = (*ml == L"1");
        if (auto bh = _db->getSetting(L"buffer_hidden")) _dataFlowOn = (*bh == L"0");  // absent: off on mac
        if (auto fc = _db->getSetting(L"buffer_fluid_color"); fc && fc->size() == 6) {
            wchar_t* end = nullptr;
            const unsigned long v = std::wcstoul(fc->c_str(), &end, 16);
            if (end && *end == L'\0') REDataFlowSetFluidColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        }
        if (auto sd = _db->getSetting(L"buffer_scaled_dots")) REDataFlowSetScaledDots(*sd == L"1");
        if (auto gs = _db->getSetting(L"meter_glass"); gs && !gs->empty())  // absent = 0 (off)
            REMeterSetGlass(std::wcstof(gs->c_str(), nullptr));
        if (auto x = _db->getSetting(L"meter_pos_x")) _meterPosX = std::clamp(std::wcstod(x->c_str(), nullptr), 0.0, 1.0);
        if (auto y = _db->getSetting(L"meter_pos_y")) _meterPosY = std::clamp(std::wcstod(y->c_str(), nullptr), 0.0, 1.0);
        // PiP inset geometry (position fraction + size px). Size is re-clamped against the
        // live container in -applyVideoPaneLayout, so a stale size from a bigger window is safe.
        if (auto m = _db->getSetting(L"pip_moved")) _pipMoved = (*m == L"1");
        if (auto x = _db->getSetting(L"pip_pos_x")) _pipPosX = std::clamp(std::wcstod(x->c_str(), nullptr), 0.0, 1.0);
        if (auto y = _db->getSetting(L"pip_pos_y")) _pipPosY = std::clamp(std::wcstod(y->c_str(), nullptr), 0.0, 1.0);
        if (auto w = _db->getSetting(L"pip_w")) _pipW = std::max(0.0, std::wcstod(w->c_str(), nullptr));
        if (auto h = _db->getSetting(L"pip_h")) _pipH = std::max(0.0, std::wcstod(h->c_str(), nullptr));
        // Recording container: ts (default, crash-safest) or mp4. (No mkv — the bundled VLC
        // has no mkv muxer; see +extForFormat:.)
        _recFormat = L"ts";
        if (auto r = _db->getSetting(L"rec_format"))
            if (*r == L"mp4" || *r == L"ts") _recFormat = *r;

        // Resume last channel on launch (Win32 parity, default ON). Stored "1"/"0"; unset => on.
        _resumeLast = YES;
        if (auto rl = _db->getSetting(L"resume_last")) _resumeLast = (*rl == L"1");

        // Categories include-filter (newline-delimited group-titles; empty = show all).
        _categoryFilter.clear();
        if (auto cf = _db->getSetting(L"category_filter"); cf && !cf->empty()) {
            const std::wstring& s = *cf;
            size_t pos = 0;
            while (pos <= s.size()) {
                const size_t nl = s.find(L'\n', pos);
                std::wstring g = s.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
                if (!g.empty()) _categoryFilter.insert(g);
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
        }

        _hideDead = false;  // Hide unavailable channels (default off — opt-in).
        if (auto hd = _db->getSetting(L"hide_dead")) _hideDead = (*hd == L"1");
    }
}

// Push the loaded config into the views, relayout, and match the Spectrum tap to its
// enabled state (creating the tap is what triggers the one-time consent prompt).
// The top-edge drag (DraggableMeterBar): the meters' height follows the bar's wanted height, the
// bar's bottom staying where it is. Any dp from 30 to 120 (Win32's range); persisted on mouse-up.
- (void)resizeMetersToBarHeight:(CGFloat)barH {
    namespace ml = rabbitears::mac;
    const CGFloat labelH = _meterLabels ? ml::kMeterLabelRowPt : 0;
    int dp = ml::meterHeightDpForPt(std::max<CGFloat>(1, barH - (barH > kMeterH + labelH ? labelH : 0)));
    if (dp == _meterHeightDp) return;
    const CGFloat bottom = _meterBar.frame.origin.y;
    _meterHeightDp = dp;
    [self applyMeterLayout];
    // Keep the bottom edge where the drag started: re-derive the position fraction from it.
    NSView* pane = _videoContainer;
    const CGFloat availY = std::max(1.0, pane.bounds.size.height - _meterBar.frame.size.height);
    _meterPosY = std::clamp(bottom / availY, 0.0, 1.0);
    [self applyMeterLayout];
    [_meterBridge applyConfigs:_meterCfg labels:_meterLabels tank:_dataFlowOn];
}

- (void)persistMeterHeight {
    if (_db) _db->setSetting(L"meter_height", std::to_wstring(_meterHeightDp));
}

// Settings ▾ ▸ Meters ▸ Standard size / Large / Extra large (Win32 MenuMeterSize*).
- (void)selectMeterSize:(NSMenuItem*)sender {
    _meterHeightDp = rabbitears::mac::clampMeterHeightDp((int)sender.tag);
    [self persistMeterHeight];
    [self applyMeterLayout];
    [self persistMeterPos];
}

// Settings ▾ ▸ Meters ▸ the Data flow checkbox (via the dialog), or the tank's right-click "Hide".
- (void)setDataFlowOn:(BOOL)on {
    _dataFlowOn = on;
    if (_db) _db->setSetting(L"buffer_hidden", on ? L"0" : L"1");
    [self applyMeterLayout];
    [_meterBridge applyConfigs:_meterCfg labels:_meterLabels tank:_dataFlowOn];
}

// Settings ▾ ▸ Meters ▸ Data-flow dots grow with the meter (Win32 buffer_scaled_dots, off by default).
- (void)toggleTankDots:(id)__unused sender {
    const bool on = !REDataFlowScaledDots();
    REDataFlowSetScaledDots(on);
    if (_db) _db->setSetting(L"buffer_scaled_dots", on ? L"1" : L"0");
    [_tank setNeedsDisplay:YES];
}

// Settings ▾ ▸ Meters ▸ Meter labels.
- (void)toggleMeterLabels:(id)__unused sender {
    _meterLabels = !_meterLabels;
    if (_db) _db->setSetting(L"meter_labels", _meterLabels ? L"1" : L"0");
    [self applyMeterLayout];
    [_meterBridge applyConfigs:_meterCfg labels:_meterLabels tank:_dataFlowOn];
}

// Settings ▾ ▸ Meters ▸ Meter bridge window: a separate window whose meters MIRROR the tray's.
- (void)toggleMeterBridge:(id)__unused sender {
    if (_meterBridge && [_meterBridge isVisible]) [self closeMeterBridge:YES];
    else [self openMeterBridge];
}

- (void)openMeterBridge {
    if (!_meterBridge) {
        _meterBridge = [[MeterBridgeWindowController alloc] init];  // MRC: app-lifetime, +1 held
        MainWindowController* __unsafe_unretained me = self;
        _meterBridge.onClose = ^{ [me closeMeterBridge:NO]; };
    }
    [_meterBridge applyConfigs:_meterCfg labels:_meterLabels tank:_dataFlowOn];
    for (int k = 0; k < 4; ++k) [_meters[k] setMirror:[_meterBridge meterForKind:k]];
    [_tank setMirror:[_meterBridge tank]];
    [_meterBridge showNear:_window];
    if (_db) _db->setSetting(L"meter_bridge", L"1");
}

// `byMenu`: the window is still up (the menu toggled it off) — order it out too.
- (void)closeMeterBridge:(BOOL)byMenu {
    for (int k = 0; k < 4; ++k) [_meters[k] setMirror:nil];  // unlinked BEFORE the twin can go
    [_tank setMirror:nil];
    if (byMenu) [_meterBridge close];
    if (_db) _db->setSetting(L"meter_bridge", L"0");
}

- (void)applyMeterConfig {
    for (int k = 0; k < 4; ++k) [_meters[k] setConfig:_meterCfg[k]];
    [_meterBridge applyConfigs:_meterCfg labels:_meterLabels tank:_dataFlowOn];
    [self applyMeterLayout];
    if (_meterCfg[(int)MeterKind::Spectrum].enabled) {
        [_meters[(int)MeterKind::Spectrum] setAvailable:YES];
        [self startSpectrumTap];
    } else {
        [self stopSpectrumTap];
    }
}

// Lay the enabled meters out on one line at the bottom-left (each a fixed per-kind
// width); the video fills the space above. All hidden while video-only.
- (void)applyMeterLayout {
    NSView* pane = _videoContainer;
    if (!pane) return;
    const CGFloat W = pane.bounds.size.width, H = pane.bounds.size.height;
    // (the video panes are positioned by -applyVideoPaneLayout; the meter bar floats on top)

    // Lay the enabled meters left-to-right inside the bar; size the bar to fit them. At the standard
    // height every meter keeps its kind's width and the gap (the tray as it always was); taller, the
    // Win32 own row's rules (MeterLayout.h): widths scale, needle looks take their instrument's own
    // width, no gap, and the optional labels' row under them.
    namespace ml = rabbitears::mac;
    const bool ownRow = _meterHeightDp > ml::kMeterHeightStd;
    // Never taller than the pane allows (the video keeps at least half), so a small window keeps the bar.
    const CGFloat labelH = (ownRow && _meterLabels) ? ml::kMeterLabelRowPt : 0;
    const CGFloat h = std::min(ml::meterHeightPt(_meterHeightDp), std::max(kMeterH, H * 0.5 - labelH));
    const CGFloat gap = ownRow ? 0 : kMeterGap;
    const BOOL tankShow = _dataFlowOn && !_videoOnly && !_metersHidden;
    // Which meters fit the pane. Win32 lays the tray out right-to-left with the tank pinned at the
    // right and HIDES a meter that does not fit (MainWindowChrome), so the row never runs off the
    // video. mac laid out left-to-right with no width check: at Extra large — or a dragged height —
    // in an ordinary window the row outgrew the pane and the bar was clipped, losing part of Frames
    // and the whole tank (found on device, 2026-11). Same rule here: the tank first, then each
    // meter from the right (Frames → Spectrum) if it still fits; a narrower one further left may.
    BOOL fits[4] = {NO, NO, NO, NO};
    {
        const CGFloat tankW = tankShow ? ml::tankWidthPt(h, ownRow) : 0;
        CGFloat metersW = 0;
        int placed = 0;
        auto rowW = [&](CGFloat mw, int n) {  // n meters of total width mw, + the gaps, + the tank
            CGFloat w = mw + (n > 1 ? gap * (n - 1) : 0);
            if (tankShow) w += (n > 0 ? std::max(gap, kMeterGap) : 0) + tankW;
            return w;
        };
        for (int k = 3; k >= 0; --k) {
            if (!(_meterCfg[k].enabled && !_videoOnly && !_metersHidden)) continue;
            const CGFloat w = ml::meterWidthPt(k, _meterCfg[k].style, h, ownRow);
            if (rowW(metersW + w, placed + 1) <= W) {
                fits[k] = YES;
                metersW += w;
                ++placed;
            }
        }
    }
    CGFloat x = 0;
    for (int k = 0; k < 4; ++k) {
        const BOOL show = fits[k];
        _meters[k].hidden = !show;
        _meterLabelViews[k].hidden = !show || labelH <= 0;
        if (!show) continue;
        const CGFloat w = ml::meterWidthPt(k, _meterCfg[k].style, h, ownRow);
        _meters[k].frame = NSMakeRect(x, labelH, w, h);
        if (labelH > 0) {
            _meterLabelViews[k].attributedStringValue = ml::meterLabelText(k);
            _meterLabelViews[k].frame = NSMakeRect(x, 0, w, labelH);
        }
        x += w + gap;
    }
    // The data-flow tank, right-most, keeping its distance from the meters in either row (Win32).
    _tank.hidden = !tankShow;
    _tankLabel.hidden = !tankShow || labelH <= 0;
    if (tankShow) {
        if (x > 0 && gap < kMeterGap) x += kMeterGap - gap;  // an own row's meters touch; the tank does not
        const CGFloat w = ml::tankWidthPt(h, ownRow);
        _tank.frame = NSMakeRect(x, labelH, w, h);
        if (labelH > 0) {
            _tankLabel.attributedStringValue = ml::meterLabelText(4);
            _tankLabel.frame = NSMakeRect(x, 0, w, labelH);
        }
        x += w + gap;
    }
    const CGFloat barW = x > 0 ? x - gap : 0;  // drop the trailing gap
    _meterBar.hidden = (barW <= 0);
    if (barW <= 0) return;
    const CGFloat barH = h + labelH;
    // Position the bar at the persisted fraction of the available range, clamped.
    const CGFloat availX = std::max(0.0, W - barW), availY = std::max(0.0, H - barH);
    _meterBar.frame = NSMakeRect(_meterPosX * availX, _meterPosY * availY, barW, barH);
    [_meterBar.window invalidateCursorRectsForView:_meterBar];
}

- (void)videoPaneResized:(NSNotification*)__unused n { [self applyVideoPaneLayout]; [self applyMeterLayout]; }

- (void)contentFrameChanged:(NSNotification*)__unused n { [self layoutBottomBar]; }

// Lay out the bottom strip: [status ......] [⏸ ⏪ ==== ⏩ 12:34 / 1:45:00] [meter] [mute] [vol]
//
// ⚠ THIS EXISTS BECAUSE THE STATUS FIELD HAS NO SLACK. Its frame used to be assigned exactly
// once, with NSViewWidthSizable doing the rest — fine while the right-hand cluster was a fixed
// 150 pt. The transport cluster is not fixed: it is only present for seekable media, so a
// permanent reserve would steal ~330 pt from the status line of every live-TV user for a
// control they never see. At the 560 pt contentMinSize the status field is ALREADY flush with
// the meter button, so there is nothing to borrow. Hence: compute both here, and re-run on
// every content resize (-contentFrameChanged:) and every visibility flip.
//
// The status line is not decorative — it carries the sticky grid-truncation notice, so
// truncating IT is a real loss of information.
- (void)layoutBottomBar {
    if (!_status) return;                       // called before the bar exists
    const CGFloat w = _window.contentView.bounds.size.width;
    const CGFloat statusH = kStatusH;
    // ⚠ 162, NOT 150. This is _meterBtn's left edge (`cs.width - 162`, where it is built) and so
    // the true left edge of the meter+mute+volume cluster. The pre-existing _status formula got
    // away with 150 only because it also subtracted 24, landing its right edge at exactly
    // w-162 — flush with the button. A review caught the transport inheriting the 150 without
    // that compensation, which put the right-aligned readout 8 pt ON TOP of the meter toggle at
    // every window width: _seekTime is added to the content view after _meterBtn, so it won
    // hit-testing too and swallowed clicks on the button's leftmost third whenever a seekable
    // stream was playing. Derive both edges from ONE constant so they cannot drift again.
    const CGFloat rightClusterW = kRightClusterW;
    CGFloat transportRight = w - rightClusterW; // left edge of the right-hand cluster
    CGFloat statusRight = transportRight;       // flush with the cluster, exactly as it shipped

    if (_seekShown) {
        // Fixed-width pieces; the slider absorbs the remainder, floored so it never inverts.
        const CGFloat btnW = 22, gap = 4, timeW = 104;
        const CGFloat wanted = btnW * 3 + gap * 4 + timeW + 120;   // 120 = minimum useful slider
        // Never take more than half the bar, so the status line keeps a readable share on a
        // narrow window; the slider shrinks first.
        const CGFloat avail = MIN(wanted + 200, MAX(wanted, (w - 24) * 0.55));
        const CGFloat x0 = transportRight - avail;
        CGFloat x = x0;
        _pauseBtn.frame    = NSMakeRect(x, 1, btnW, 20);            x += btnW + gap;
        _skipBackBtn.frame = NSMakeRect(x, 1, btnW, 20);            x += btnW + gap;
        const CGFloat sliderW = MAX(60, transportRight - timeW - gap * 2 - btnW - gap - x);
        _seek.frame        = NSMakeRect(x, 2, sliderW, 18);         x += sliderW + gap;
        _skipFwdBtn.frame  = NSMakeRect(x, 1, btnW, 20);            x += btnW + gap;
        _seekTime.frame    = NSMakeRect(x, 3, transportRight - x - gap, statusH - 5);
        statusRight = x0 - 12;                   // the status field now ends 12 pt before it
    }
    _status.frame = NSMakeRect(12, 3, MAX(40, statusRight - 12), statusH - 5);
}

// Format a ms position as m:ss, or h:mm:ss once the media is an hour or longer.
static NSString* fmtPos(long long ms, long long lenMs) {
    if (ms < 0) ms = 0;
    const long long s = ms / 1000;
    const long long h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (lenMs >= 3600000)
        return [NSString stringWithFormat:@"%lld:%02lld:%02lld", h, m, sec];
    return [NSString stringWithFormat:@"%lld:%02lld", s / 60, sec];
}

// Show/hide + refresh the transport from the active pane. Called from -tickStats.
- (void)updateTransport {
    VlcPlayerMac* p = [self activePlayer];
    const long long len  = p ? p->lengthMs() : 0;
    // hasActiveInput() FIRST — see its header note. At libvlc_Ended the length and seekability
    // both persist and only get_time drops to 0, so a length+seekable gate can never retire the
    // bar: an on-device pass found a finished film leaving the transport stranded with a frozen
    // 0:00 readout and no way back. The ~2 s hide hysteresis below now covers this condition too,
    // which is what keeps an HLS mid-stream reopen (a brief Opening/NothingSpecial dip) from
    // flapping the bar.
    const BOOL want = (p && p->hasActiveInput() && p->isSeekable() && len > 0) ? YES : NO;

    // HYSTERESIS ON HIDE, immediate on show. A live HLS input that re-opens mid-stream
    // (segment discontinuity, rendition switch, reconnect) briefly has no input thread, so
    // get_length returns -1 → 0 → `want` flips false for a tick or two. Retiring the cluster on
    // a single sample would make the whole bottom bar twitch at 4 Hz on ordinary live TV, and
    // every flip re-frames the status line. Win32 hides this inside one DeferWindowPos batch;
    // mac has no such batch, so the fix is to not flap in the first place.
    if (want) _seekHideTicks = 0;
    else if (_seekShown && ++_seekHideTicks < 8) return;   // ~2 s of steady "no" before hiding

    if (want != _seekShown) {
        _seekShown = want;
        const BOOL hide = !want || _videoOnly;
        _pauseBtn.hidden = _skipBackBtn.hidden = _seek.hidden = hide;
        _skipFwdBtn.hidden = _seekTime.hidden = hide;
        [self layoutBottomBar];                 // the status field reclaims/yields the width
    }
    if (!want) return;

    // Post-seek latch: libVLC keeps reporting the OLD position for a moment after set_time, so
    // a sample landing in that window would snap the thumb back to where the user dragged from.
    // The latch is scoped to the pane generation it was created for, so switching panes (or
    // tearing one down) can never leave pane A's target driving pane B's thumb.
    const long long nowMs = (long long)(NSDate.timeIntervalSinceReferenceDate * 1000.0);
    const BOOL latched = _seekLatchUntilMs > nowMs && _seekLatchPaneId == _panesGeneration;
    const long long pos = _seekDragging ? (long long)_seek.doubleValue
                        : latched       ? _seekLatchTarget
                                        : p->timeMs();

    if (!_seekDragging) {
        _seek.maxValue = (double)len;
        _seek.doubleValue = (double)MIN(pos, len);
    }
    _seekTime.stringValue = [NSString stringWithFormat:@"%@ / %@",
                                                       fmtPos(pos, len), fmtPos(len, len)];
    // Reflect libVLC's own pause state rather than a local flag, so a stream that refused to
    // pause (or ended while paused) can't leave the wrong glyph showing.
    const BOOL paused = p->isPaused();
    NSString* sym = paused ? @"play.fill" : @"pause.fill";
    _pauseBtn.image = [NSImage imageWithSystemSymbolName:sym
                               accessibilityDescription:Tr(paused ? StringId::LabelPlay
                                                                  : StringId::TooltipBtnPause)];
    _pauseBtn.toolTip = Tr(paused ? StringId::LabelPlay : StringId::TooltipBtnPause);
}

// ---- transport actions ----------------------------------------------------------------

- (void)togglePause:(id)__unused sender {
    if (VlcPlayerMac* p = [self activePlayer]) p->setPaused(!p->isPaused());
}

- (void)seekChanged:(id)__unused sender {
    // NSSlider is continuous, so this fires throughout the drag. Track the thumb, but COMMIT
    // only on mouse-up: seeking a network stream re-buffers, and seeking per drag tick thrashes
    // the demuxer. NSApp.currentEvent is the drag/So-up discriminator AppKit gives us here.
    const NSEventType t = NSApp.currentEvent.type;
    if (t == NSEventTypeLeftMouseDown || t == NSEventTypeLeftMouseDragged) {
        _seekDragging = YES;
        // Live text feedback while dragging, without touching the player.
        if (VlcPlayerMac* p = [self activePlayer]) {
            const long long len = p->lengthMs();
            _seekTime.stringValue = [NSString stringWithFormat:@"%@ / %@",
                                     fmtPos((long long)_seek.doubleValue, len), fmtPos(len, len)];
        }
        return;
    }
    // Mouse-up (or a keyboard/accessibility change, which has no drag phase at all): commit.
    _seekDragging = NO;
    [self commitSeekTo:(long long)_seek.doubleValue];
}

// Drop the post-seek latch. Its real scope is "this player, still showing the media the seek was
// issued against" — NOT the pane SET, which is all _panesGeneration tracks. Anything that changes
// WHAT the transport is looking at must call this, or the latch keeps painting the old target
// over the new stream for the rest of its 3 s: a review found that clicking a second seekable
// pane inside the window put pane A's target on pane B's thumb and produced readouts like
// "1:20:00 / 45:00" (a position past the media's own duration).
- (void)invalidateSeekLatch {
    _seekLatchUntilMs = 0;
    _seekLatchTarget  = 0;
    _seekLatchPaneId  = 0;
}

// Ask the player to reposition, and latch the UI to the target briefly so the thumb does not
// snap back while libVLC still reports the old time.
- (void)commitSeekTo:(long long)ms {
    VlcPlayerMac* p = [self activePlayer];
    if (!p || !p->isSeekable()) return;
    // Latch what the player ACTUALLY took, not what we asked for. seekTo clamps (it keeps a second
    // off the end), so latching the raw request made ⏩ near the end display a current position
    // PAST the total — "1:45:05 / 1:45:00" — and then jump back when the latch expired. Returning
    // the applied position keeps the clamp rule in one place instead of duplicating it here.
    const long long applied = p->seekTo(ms);
    if (applied < 0) return;                // the player refused — nothing to latch
    _seekLatchTarget  = applied;
    _seekLatchUntilMs = (long long)(NSDate.timeIntervalSinceReferenceDate * 1000.0) + 3000;
    _seekLatchPaneId  = _panesGeneration;   // scope it to THIS pane incarnation
}

- (void)skipBack:(id)__unused sender {
    if (VlcPlayerMac* p = [self activePlayer]) [self commitSeekTo:p->timeMs() - 10000];
}

- (void)skipForward:(id)__unused sender {
    if (VlcPlayerMac* p = [self activePlayer]) [self commitSeekTo:p->timeMs() + 10000];
}

// Record the meter bar's position as a 0..1 fraction of the pane (called after a drag).
- (void)persistMeterPos {
    NSView* pane = _videoView.superview;
    if (!pane) return;
    const CGFloat availX = std::max(1.0, pane.bounds.size.width - _meterBar.frame.size.width);
    const CGFloat availY = std::max(1.0, pane.bounds.size.height - _meterBar.frame.size.height);
    _meterPosX = std::clamp(_meterBar.frame.origin.x / availX, 0.0, 1.0);
    _meterPosY = std::clamp(_meterBar.frame.origin.y / availY, 0.0, 1.0);
    if (_db) {
        _db->setSetting(L"meter_pos_x", std::to_wstring(_meterPosX));
        _db->setSetting(L"meter_pos_y", std::to_wstring(_meterPosY));
    }
}

// ---- Multi-view (Single / Split-2×2) -----------------------------------------
// Panes are laid out by common/ui/VideoGrid (the shared geometry Win32 uses too) and hung
// in the video container. Only the ACTIVE pane produces audio + drives the meters; clicking
// a pane activates it. A mode switch carries the active stream into pane 0 and tears surplus
// panes down OFF the main thread (libVLC stop() is synchronous — a stuck stream would freeze).

- (ViewMode)viewMode { return _viewMode; }
- (BOOL)isSplitView { return _viewMode == ViewMode::Split; }
- (BOOL)isPipView { return _viewMode == ViewMode::Pip; }

// Build a new pane (view + player) and add it to the container, below the meter bar.
- (MacVideoPane*)makePane {
    auto pane = std::make_unique<MacVideoPane>();
    pane->player = std::make_unique<VlcPlayerMac>();
    pane->player->init(*_engine);
    NSView* v = [[NSView alloc] initWithFrame:_videoContainer.bounds];
    v.autoresizingMask = NSViewNotSizable;  // positioned by -applyVideoPaneLayout
    v.wantsLayer = YES;
    v.layer.backgroundColor = NSColor.blackColor.CGColor;
    NSClickGestureRecognizer* clk =
        [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(paneClicked:)];
    [v addGestureRecognizer:clk];
    [clk release];  // MRC: the view retains it
    NSPanGestureRecognizer* pan =  // drags the PiP inset (a no-op in Single/Split)
        [[NSPanGestureRecognizer alloc] initWithTarget:self action:@selector(paneDragged:)];
    [v addGestureRecognizer:pan];
    [pan release];
    v.menu = _videoMenu;  // same right-click view menu on every pane
    [_videoContainer addSubview:v positioned:NSWindowBelow relativeTo:_meterBar];  // under the meters
    [v release];    // MRC: the container retains it now
    pane->view = v;
    pane->player->attachTo(v);
    MacVideoPane* raw = pane.get();
    _panes.push_back(std::move(pane));
    return raw;
}

// Position every pane inside the container per the current mode (VideoGrid geometry,
// y-flipped for AppKit's bottom-left origin). A zero-area box hides a surplus pane.
- (void)applyVideoPaneLayout {
    if (!_videoContainer || _panes.empty()) return;
    // Flip against the SAME integer height fed to computeVideoPanes, else the container's
    // fractional height leaks in as a sub-pixel gap along the bottom edge.
    const int icw = (int)_videoContainer.bounds.size.width;
    const int ich = (int)_videoContainer.bounds.size.height;
    rabbitears::VideoGridOpts opts;
    // PiP inset size: the user's persisted size if set, else the default. Clamp to the
    // container so a stale saved size (from a larger window) can't exceed it.
    const int pw = _pipW > 0 ? (int)_pipW : kPipDefaultW;
    const int ph = _pipH > 0 ? (int)_pipH : kPipDefaultH;
    opts.gap = 3;
    opts.pipW = std::clamp(pw, kPipMinW, std::max(kPipMinW, (int)(icw * 0.6)));
    opts.pipH = std::clamp(ph, kPipMinH, std::max(kPipMinH, (int)(ich * 0.6)));
    opts.pipMargin = 14;
    auto boxes = rabbitears::computeVideoPanes(_viewMode, (int)_panes.size(), 0, 0, icw, ich, opts);
    for (size_t i = 0; i < _panes.size() && i < boxes.size(); ++i) {
        NSView* v = _panes[i]->view;
        const auto& b = boxes[i];
        if (b.w <= 0 || b.h <= 0) { v.hidden = YES; continue; }
        v.hidden = NO;
        NSRect fr = NSMakeRect(b.x, ich - b.y - b.h, b.w, b.h);  // top-down box → bottom-up frame
        // A PiP inset the user dragged keeps its position (as a fraction) across relayout.
        if (_viewMode == ViewMode::Pip && i > 0 && _pipMoved) {
            fr.origin.x = _pipPosX * std::max<CGFloat>(0, icw - fr.size.width);
            fr.origin.y = _pipPosY * std::max<CGFloat>(0, ich - fr.size.height);
        }
        v.frame = fr;
    }
    [self updateActivePaneBorder];
}

// An accent border around the active pane (only when there's more than one).
- (void)updateActivePaneBorder {
    const BOOL multi = _panes.size() > 1;
    for (int i = 0; i < (int)_panes.size(); ++i) {
        CALayer* layer = _panes[(size_t)i]->view.layer;
        if (multi && i == _activePane) {
            layer.borderColor = NSColor.controlAccentColor.CGColor;
            layer.borderWidth = 3;
        } else {
            layer.borderWidth = 0;
        }
    }
}

// Make pane `idx` the audio/meter/volume focus. Background panes are muted by audio-track
// deselect (VlcPlayerMac::setMuted), so only one pane is ever audible.
- (void)setActivePane:(int)idx {
    if (idx < 0 || idx >= (int)_panes.size()) idx = 0;
    _activePane = idx;
    [self invalidateSeekLatch];   // the transport is about to look at a different player
    _player = _panes[(size_t)idx]->player.get();   // re-alias: stats/spectrum/volume follow active
    _videoView = _panes[(size_t)idx]->view;
    for (int i = 0; i < (int)_panes.size(); ++i)
        _panes[(size_t)i]->player->setMuted(i != idx);   // single audio: only the active pane
    _player->setVolume((int)_volume.doubleValue);
    [self updateActivePaneBorder];
    [self updateRecordButton];  // the Record button reflects the newly-active pane's state
    [self applyMeterLayout];
    MacVideoPane* p = [self activePane];
    if (p && p->channelId) {
        _window.title = TrF(StringId::MacMainWindowTitleWithChannel, {ns(p->channel.name)});
        [self setStatus:TrF(StringId::MacMainWindowActivePaneChannel, {ns(p->channel.name)})];
    }
}

- (void)paneClicked:(NSClickGestureRecognizer*)g {
    for (int i = 0; i < (int)_panes.size(); ++i)
        if (_panes[(size_t)i]->view == g.view) { [self setActivePane:i]; return; }
}

// Drag the PiP inset around its backdrop (pane 0 is the full-bleed backdrop and never moves).
// A drag that STARTS in the inset's top-left grip resizes it (bottom-right corner pinned);
// any other drag moves it. Position is stored as a 0..1 fraction of the free travel and size
// in px, both persisted, so the inset survives relayout AND relaunch.
- (void)paneDragged:(NSPanGestureRecognizer*)g {
    if (_viewMode != ViewMode::Pip || !_videoContainer) return;
    int idx = -1;
    for (int i = 0; i < (int)_panes.size(); ++i)
        if (_panes[(size_t)i]->view == g.view) { idx = i; break; }
    if (idx <= 0) return;  // only inset panes move/resize (pane 0 is the fixed backdrop)
    NSView* v = g.view;

    if (g.state == NSGestureRecognizerStateBegan) {
        // Decide resize-vs-move ONCE, from where the drag started. The inset view is not
        // flipped (origin bottom-left), so its top-left corner is (0, height) — the corner
        // that faces into the screen and is the natural resize handle.
        const NSPoint p = [g locationInView:v];
        const CGFloat grip = 24;
        _pipResizing = (p.x <= grip && p.y >= v.bounds.size.height - grip);
        // Baseline for the aspect snap. Latched at drag START, like Win32's pipResizeOrigin, and
        // paired with _pipResizeAccum below: the resize branch rebuilds the size from
        // origin + CUMULATIVE RAW PULL rather than stepping the live frame, so the snap's own
        // output can never become the next step's input.
        _pipResizeOriginSize = v.frame.size;
        _pipResizeAccum = NSZeroPoint;
        [g setTranslation:NSZeroPoint inView:_videoContainer];
        return;
    }

    const NSPoint d = [g translationInView:_videoContainer];
    [g setTranslation:NSZeroPoint inView:_videoContainer];
    const CGFloat cw = _videoContainer.bounds.size.width;
    const CGFloat ch = _videoContainer.bounds.size.height;
    NSRect f = v.frame;

    if (_pipResizing) {
        // Top-left handle: pin the bottom-right corner, grow toward the top-left. Dragging
        // left (d.x<0) widens; dragging up (d.y>0) heightens. The upper bound is the SMALLER
        // of 60% of the container and the free space to the pinned corner (rightX to the left,
        // ch-bottomY upward) — so the growing top/left edge can never leave the container.
        // (Both free spaces are >= the min: the inset is on-screen and at least min-sized.)
        const CGFloat rightX  = f.origin.x + f.size.width;
        const CGFloat bottomY = f.origin.y;
        const CGFloat maxW = std::max<CGFloat>(kPipMinW, std::min<CGFloat>(cw * 0.6, rightX));
        const CGFloat maxH = std::max<CGFloat>(kPipMinH, std::min<CGFloat>(ch * 0.6, ch - bottomY));
        // Rebuild the size from the LATCHED origin plus the cumulative raw pull, never by stepping
        // the live frame. Stepping the frame fed the snap's own output back in below, and because
        // the width branch sets newH = newW/aspect it then satisfied its own dw >= dh test on the
        // next event and latched there: a near-vertical drag froze after ~4 events and the grip
        // detached from the cursor (reproduced numerically by two independent reviewers, at every
        // drag speed). Win32 never had this because it recomputes nw/nh from
        // pipResizeOrigin + (cursor - pipResizeStart) every mouse-move; this is that shape.
        _pipResizeAccum.x += d.x;
        _pipResizeAccum.y += d.y;
        CGFloat newW = std::clamp<CGFloat>(_pipResizeOriginSize.width  - _pipResizeAccum.x, kPipMinW, maxW);
        CGFloat newH = std::clamp<CGFloat>(_pipResizeOriginSize.height + _pipResizeAccum.y, kPipMinH, maxH);

        // SNAP TO THE STREAM'S ASPECT RATIO. A free-form inset letterboxes itself: libVLC fits the
        // picture inside the view preserving aspect, so any mismatch shows up as black bars down
        // one side (owner-reported against Win32 in 0.2.14). Drive off the DOMINANT EDGE — whichever
        // dimension the user has pulled further from the size this drag started at — so the drag
        // still feels like it follows the cursor instead of fighting it.
        //
        // videoSize() is the getter the seek layer added; this is its first consumer. It reports
        // 0x0 until the vout is up, and for an audio-only stream it never reports anything — both
        // fall back to 16:9 rather than to free-form, so the inset has a sane shape before the
        // first frame decodes. Re-clamped after snapping: the snap can push the driven dimension
        // past its bound, and letting that through would walk the inset off the container edge.
        unsigned vw = 0, vh = 0;
        _panes[(size_t)idx]->player->videoSize(vw, vh);
        const double aspect = (vw && vh) ? (double)vw / (double)vh : 16.0 / 9.0;
        if (aspect > 0.01) {
            // Dominance measured on the USER'S PULL, not on the frame — the frame is what the snap
            // already distorted, and testing it against itself is what caused the freeze.
            const CGFloat dw = std::abs(_pipResizeAccum.x);
            const CGFloat dh = std::abs(_pipResizeAccum.y);
            if (dw >= dh) newH = (CGFloat)(newW / aspect + 0.5);
            else          newW = (CGFloat)(newH * aspect + 0.5);
            newW = std::clamp<CGFloat>(newW, kPipMinW, maxW);
            newH = std::clamp<CGFloat>(newH, kPipMinH, maxH);
        }
        f.size.width = newW; f.size.height = newH;
        // Bottom-right pinned. newW<=rightX and newH<=ch-bottomY already keep both edges
        // on-screen for any container >= the min inset; the max(0,...) floors make that hold
        // by construction even if the container were ever smaller than the inset minimum.
        f.origin.x = std::max<CGFloat>(0, rightX - newW);
        f.origin.y = std::max<CGFloat>(0, bottomY);
        v.frame = f;
        _pipW = newW; _pipH = newH;
    } else {
        const CGFloat maxX = std::max<CGFloat>(0, cw - f.size.width);
        const CGFloat maxY = std::max<CGFloat>(0, ch - f.size.height);
        f.origin.x = std::clamp<CGFloat>(f.origin.x + d.x, 0, maxX);
        f.origin.y = std::clamp<CGFloat>(f.origin.y + d.y, 0, maxY);
        v.frame = f;
    }

    // Recompute the position fraction from the (possibly resized) frame so a later relayout
    // reproduces this exact spot. Clamped defensively — the geometry above already keeps the
    // frame on-screen, so these are always in [0,1]; the clamp guards against any future edit
    // writing an out-of-range fraction that -loadMeterConfig would then snap on relaunch.
    const CGFloat maxX = std::max<CGFloat>(0, cw - f.size.width);
    const CGFloat maxY = std::max<CGFloat>(0, ch - f.size.height);
    _pipPosX = std::clamp<CGFloat>(maxX > 0 ? f.origin.x / maxX : 0, 0, 1);
    _pipPosY = std::clamp<CGFloat>(maxY > 0 ? f.origin.y / maxY : 0, 0, 1);
    _pipMoved = YES;

    if (g.state == NSGestureRecognizerStateEnded ||
        g.state == NSGestureRecognizerStateCancelled)
        [self persistPipGeometry];
}

// Persist the PiP inset geometry (peer of the meter-bar position persistence). Fractions for
// position (window-size-independent), px for size (clamped back into range on load).
- (void)persistPipGeometry {
    if (!_db) return;
    _db->setSetting(L"pip_moved", _pipMoved ? L"1" : L"0");
    _db->setSetting(L"pip_pos_x", std::to_wstring((double)_pipPosX));
    _db->setSetting(L"pip_pos_y", std::to_wstring((double)_pipPosY));
    _db->setSetting(L"pip_w", std::to_wstring((double)_pipW));
    _db->setSetting(L"pip_h", std::to_wstring((double)_pipH));
}

// Copy the channel playing in `from` into pane `to` — used when collapsing so the active
// stream survives into Single view (peer of the Win32 carryStream).
- (void)carryStreamFromPane:(int)from toPane:(int)to {
    if (from == to || from < 0 || to < 0 ||
        from >= (int)_panes.size() || to >= (int)_panes.size()) return;
    // This ends in dst->player->play() below, which claims a connection just as -playChannel:
    // does — and a view-mode collapse is a common way to reach it without ever going through
    // -playChannel:. Cancel here too, or the sync survives a carry and kicks the carried stream.
    rabbitears::mac::cancelVodSync();
    // Same rule, same reason: the sweep is the OTHER provider-facing worker, and its probes are
    // worse to leave running than the sync's downloads — a probe refused for connection capacity
    // reads as an unambiguous rejection and persists dead_status=Dead on a working channel.
    rabbitears::mac::cancelDeadLinkSweep();
    MacVideoPane* src = _panes[(size_t)from].get();
    MacVideoPane* dst = _panes[(size_t)to].get();
    if (src->channelId == 0) return;                 // nothing playing to carry
    if (dst->channelId == src->channelId) {          // already the same stream — don't re-buffer
        dst->channel = src->channel;
        return;
    }
    dst->player->play(src->channel.streamUrl, src->channel.userAgent, src->channel.referrer);
    dst->player->setVolume((int)_volume.doubleValue);
    dst->channel = src->channel;
    dst->channelId = src->channelId;
}

// Tear a removed pane down WITHOUT blocking the UI: unhook its view on the main thread, then
// stop + destroy its (possibly stuck) player on a background queue.
//
// The view must OUTLIVE the player: libVLC still holds it via set_nsobject and its vout can
// render into it until the player is actually released. removeFromSuperview drops the
// container's retain, so we take our own first and release it (back on the main thread —
// NSView dealloc is main-thread-only) once the player is gone.
- (void)teardownPane:(std::unique_ptr<MacVideoPane>)pane {
    NSView* view = [pane->view retain];  // outlive the player's vout
    [view removeFromSuperview];
    pane->view = nil;
    VlcPlayerMac* dying = pane->player.release();  // take the player out of the unique_ptr
    if (!dying) { [view release]; return; }
    // Count the detached player for the whole background stop. Once it leaves _panes nothing can
    // ask it isEngaged()/isRecording() any more, but its connection to the provider stays open
    // until the destructor returns — and libVLC's stop() is exactly the call that can take
    // seconds on a stuck feed. The VOD sync gate reads this count; without it, collapsing a 2×2
    // and immediately syncing would start against up to three still-open sockets.
    rabbitears::vlcDetachedPlayerBegin();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        dying->stop();
        delete dying;  // ~VlcPlayerMac: media_player_stop + release (blocking, off-main)
        rabbitears::vlcDetachedPlayerEnd();
        dispatch_async(dispatch_get_main_queue(), ^{ [view release]; });  // now safe to free
    });
}

// Switch view mode. Split uses `count` panes (2×2 = 4). Grows/shrinks the pane set,
// carrying the active stream into pane 0 when collapsing away from it.
- (void)applyViewMode:(ViewMode)mode paneCount:(int)count {
    if (mode == ViewMode::Single) count = 1;
    if (count < 1) count = 1;
    // Capture the mode we are LEAVING before overwriting it — the collapse rule below depends on
    // it. (Win32 reads `st->viewMode` for the same test at MainWindowCommands.cpp:780-781, where
    // it is still the old value because the assignment comes later; porting that condition
    // literally to mac would test the mode being ENTERED and get it exactly backwards.)
    const ViewMode prev = _viewMode;
    _viewMode = mode;
    // NB: the PiP inset position/size is PERSISTED (see -paneDragged: / -persistPipGeometry),
    // so entering PiP restores where the user last left it rather than snapping to the corner.

    // Any change to the pane SET invalidates a post-seek latch: it was created for the pane that
    // was active THEN, and after a grow/shrink/carry the active pane may be a different stream.
    // Without this, pane A's seek target drives pane B's thumb (a Win32 review-caught defect).
    ++_panesGeneration;
    while ((int)_panes.size() < count) [self makePane];   // grow

    if ((int)_panes.size() > count) {                     // shrink
        if (_activePane >= count) {
            // In 2×2 the tiles are equal peers, so collapsing should keep the tile you had
            // selected rather than snapping back to the top-left. PiP is NOT that: the backdrop
            // is the primary and the inset a secondary overlay, so closing PiP must leave the
            // main view playing what it already was. Carrying there hijacked pane 0 with the
            // inset's channel whenever the user had clicked the inset active (Win32 parity —
            // same owner-reported bug, MainWindowCommands.cpp:774-781).
            //
            // ...UNLESS pane 0 is idle. `carryStreamFromPane:` only guards its SOURCE
            // (:1928 `if (src->channelId == 0) return;`), never its destination, so the old
            // unconditional carry could not hijack an EMPTY pane 0 — which makes a bare
            // `prev == Split` gate strictly broader than the bug it fixes. "Play in PiP" from a
            // fresh launch (or after Stop, or from a saved layout whose pane 0 is empty) leaves
            // the inset as the only thing playing; suppressing the carry there turns "exit PiP"
            // into "stop playback", with a stale window title still naming the channel.
            // _panes[0] is always valid here: this branch needs _panes.size() > count >= 1.
            if (prev == ViewMode::Split || _panes[0]->channelId == 0)
                [self carryStreamFromPane:_activePane toPane:0];
            // Reset UNCONDITIONALLY, carry or not: this pane is about to be torn down, and the
            // alias re-point below reads _activePane. Leaving it pointing at a dying pane would
            // dangle _player/_videoView — the one invariant this method exists to protect.
            _activePane = 0;
        }
        // Re-point the aliases at a SURVIVING pane BEFORE tearing anything down, so
        // _player/_videoView can never dangle at a destroyed pane (the stats timer and
        // volume slider dereference them).
        _player = [self activePlayer];
        if (MacVideoPane* ap = [self activePane]) _videoView = ap->view;
        while ((int)_panes.size() > count) {
            std::unique_ptr<MacVideoPane> pane = std::move(_panes.back());
            _panes.pop_back();
            [self teardownPane:std::move(pane)];
        }
    }
    if (_activePane >= (int)_panes.size()) _activePane = 0;

    [self applyVideoPaneLayout];
    [self setActivePane:_activePane];
    [self syncKeepAwake];  // a torn-down pane may have been recording — re-derive the assertion
    [self updateEmptyHint];
    switch (_viewMode) {
        case ViewMode::Split:
            [self setStatus:Tr(StringId::MacMainWindowSplitViewStatus)];
            break;
        case ViewMode::Pip:
            [self setStatus:Tr(StringId::MacMainWindowPipStatus)];
            break;
        default:
            [self setStatus:Tr(StringId::MacMainWindowSingleViewStatus)];
            break;
    }
}

- (void)setViewSingle:(id)__unused sender { [self applyViewMode:ViewMode::Single paneCount:1]; }
- (void)setViewSplit:(id)__unused sender  { [self applyViewMode:ViewMode::Split  paneCount:4]; }
- (void)setViewPip:(id)__unused sender    { [self applyViewMode:ViewMode::Pip    paneCount:2]; }

// Row context menu ▸ "Play in PiP": force PiP mode and play the clicked channel into the
// INSET, leaving the backdrop (pane 0) active and audible — mirroring the Win32 "Play in
// PIP" command. Click the inset to make it the active/audible pane.
- (void)playInPip:(id)__unused sender {
    const NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_channels.size()) return;
    const Channel c = _channels[(size_t)row];  // copy — the pane switch may reload the grid
    if (_viewMode != ViewMode::Pip) [self applyViewMode:ViewMode::Pip paneCount:2];
    if (_panes.size() < 2) return;  // PiP must have an inset
    [self playChannel:c intoPane:1];
}

// ---- Named saved layouts (Settings ▸ Layouts) --------------------------------------------
// A layout captures the multi-view arrangement so the user can restore a whole 2×2 / PiP
// setup by name. Serialized form (version-tagged, ';'-fields, ','-lists), all mac-local:
//   1;<mode>;<active>;<pipW>;<pipH>;<pipPosX>;<pipPosY>;<pipMoved>;<chanId0>,<chanId1>,...
// mode 0/1/2 = Single/Split/Pip; a channel id of 0 = an empty pane. The channel list length
// is the pane count. Parsing is defensive — a malformed string yields no layout and never
// tears down the current view.

namespace {
struct SavedLayout {
    ViewMode mode = ViewMode::Single;
    int active = 0;
    CGFloat pipW = 0, pipH = 0, pipPosX = 0, pipPosY = 0;
    BOOL pipMoved = NO;
    std::vector<long long> chans;   // one per pane; 0 == empty
};

std::vector<std::wstring> splitW(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t at = s.find(sep, pos);
        out.push_back(s.substr(pos, at == std::wstring::npos ? std::wstring::npos : at - pos));
        if (at == std::wstring::npos) break;
        pos = at + 1;
    }
    return out;
}

std::optional<SavedLayout> parseLayout(const std::wstring& blob) {
    const auto f = splitW(blob, L';');
    if (f.size() < 9 || f[0] != L"1") return std::nullopt;
    SavedLayout L;
    const int m = (int)std::wcstol(f[1].c_str(), nullptr, 10);
    if (m < 0 || m > 2) return std::nullopt;
    // Geometry fields come from a K/V blob that could be hand-edited/corrupt: reject NaN/inf so
    // a bad value can never reach an NSView frame. (The app itself only ever writes finite doubles.)
    auto finite = [](double v, double lo, double hi) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0;
    };
    L.mode     = (ViewMode)m;
    L.active   = (int)std::wcstol(f[2].c_str(), nullptr, 10);
    L.pipW     = (CGFloat)finite(std::wcstod(f[3].c_str(), nullptr), 0, 100000);
    L.pipH     = (CGFloat)finite(std::wcstod(f[4].c_str(), nullptr), 0, 100000);
    L.pipPosX  = (CGFloat)finite(std::wcstod(f[5].c_str(), nullptr), 0, 1);
    L.pipPosY  = (CGFloat)finite(std::wcstod(f[6].c_str(), nullptr), 0, 1);
    L.pipMoved = (f[7] == L"1");
    for (const auto& c : splitW(f[8], L','))
        if (!c.empty()) L.chans.push_back(std::wcstoll(c.c_str(), nullptr, 10));
    if (L.chans.empty() || L.chans.size() > (size_t)kMaxPanes) return std::nullopt;
    if (L.mode == ViewMode::Single && L.chans.size() != 1) L.chans.resize(1);
    if (L.active < 0 || L.active >= (int)L.chans.size()) L.active = 0;
    return L;
}
}  // namespace

// The saved-layout name index ("layout_names"), newline-joined, capped and de-blanked.
- (std::vector<std::wstring>)savedLayoutNames {
    std::vector<std::wstring> names;
    if (!_db) return names;
    if (auto v = _db->getSetting(L"layout_names"); v && !v->empty())
        for (auto& n : splitW(*v, L'\n'))
            if (!n.empty()) names.push_back(n);
    if (names.size() > (size_t)kMaxSavedLayouts) names.resize((size_t)kMaxSavedLayouts);
    return names;
}

- (void)storeLayoutNames:(const std::vector<std::wstring>&)names {
    if (!_db) return;
    std::wstring joined;
    for (const auto& n : names) { if (!joined.empty()) joined += L'\n'; joined += n; }
    _db->setSetting(L"layout_names", joined);
}

// Settings ▸ Layouts ▸ Save Current Layout… — capture the current multi-view arrangement.
- (void)saveLayout:(id)__unused sender {
    if (!_db) return;
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = Tr(StringId::SaveLayoutDialogTitle);
    a.informativeText = Tr(StringId::MacMainWindowSaveLayoutPrompt);
    [a addButtonWithTitle:Tr(StringId::MacMainWindowSaveButton)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
    input.stringValue = Tr(StringId::LayoutDefaultName);
    a.accessoryView = input;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;
        NSString* raw = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        // The index is newline-joined and keys are "layout_saved_<name>", so strip separators.
        NSString* name = [[raw stringByReplacingOccurrencesOfString:@"\n" withString:@" "]
                                stringByReplacingOccurrencesOfString:@"\r" withString:@" "];
        if (name.length == 0) { [self showResults:Tr(StringId::SaveLayoutDialogTitle) info:Tr(StringId::MacMainWindowEnterNamePrompt)]; return; }

        auto names = [self savedLayoutNames];
        const std::wstring wname = ws(name);
        const bool exists = std::find(names.begin(), names.end(), wname) != names.end();
        if (!exists) {
            if (names.size() >= (size_t)kMaxSavedLayouts) {
                [self showResults:Tr(StringId::MacMainWindowLayoutLimitHeading)
                             info:TrF(StringId::MacMainWindowLayoutLimitBody,
                                   {[NSString stringWithFormat:@"%d", kMaxSavedLayouts]})];
                return;
            }
            names.push_back(wname);
            [self storeLayoutNames:names];
        }  // an existing name is overwritten in place

        // Serialize the current arrangement.
        std::wstring blob = L"1;";
        blob += std::to_wstring((int)_viewMode) + L";";
        blob += std::to_wstring(_activePane) + L";";
        blob += std::to_wstring((double)_pipW) + L";" + std::to_wstring((double)_pipH) + L";";
        blob += std::to_wstring((double)_pipPosX) + L";" + std::to_wstring((double)_pipPosY) + L";";
        blob += (_pipMoved ? L"1" : L"0") + std::wstring(L";");
        for (size_t i = 0; i < _panes.size(); ++i) {
            if (i) blob += L",";
            blob += std::to_wstring(_panes[i]->channelId);
        }
        _db->setSetting(L"layout_saved_" + wname, blob);
        [self setStatus:TrF(StringId::StatusLayoutSaved, {name})];
    }];
}

// Apply a saved layout: rebuild the pane set for its view mode, replay each pane's channel,
// restore the active pane + PiP geometry. Reuses -applyViewMode: and -playChannel:intoPane:
// (which own the async-teardown / alias / single-audio invariants), so this only orchestrates.
- (void)applyLayoutClicked:(NSMenuItem*)sender {
    if (!_db) return;
    NSString* name = sender.representedObject;
    auto blob = _db->getSetting(L"layout_saved_" + ws(name));
    std::optional<SavedLayout> L = blob && !blob->empty() ? parseLayout(*blob) : std::nullopt;
    if (!L) { [self showResults:Tr(StringId::MacMainWindowLayoutApplyFailedHeading)
                            info:TrF(StringId::MacMainWindowLayoutMissingBody, {name})]; return; }

    // PiP geometry must be set BEFORE -applyViewMode: so entering PiP restores it (applyViewMode
    // no longer resets _pipMoved).
    _pipW = L->pipW; _pipH = L->pipH;
    _pipPosX = L->pipPosX; _pipPosY = L->pipPosY; _pipMoved = L->pipMoved;

    int count = std::clamp((int)L->chans.size(), 1, kMaxPanes);
    [self applyViewMode:L->mode paneCount:count];

    // Replay each pane's saved channel; a channel whose playlist was since removed is skipped.
    int missing = 0;
    for (int i = 0; i < count && i < (int)_panes.size(); ++i) {
        const long long cid = L->chans[(size_t)i];
        MacVideoPane* p = _panes[(size_t)i].get();
        if (cid == 0) {
            // The layout marks this pane empty. If it currently holds a stream — pre-existing,
            // or one -applyViewMode: carried into pane 0 on collapse — stop + clear it so the
            // applied arrangement matches what was saved (peer of -stop:'s clear). Synchronous
            // stop, like the Stop button.
            if (p->channelId != 0) { p->player->stop(); p->channel = Channel{}; p->channelId = 0; }
            continue;
        }
        if (auto ch = _db->channelById(cid)) [self playChannel:*ch intoPane:i];
        else ++missing;
    }
    [self setActivePane:std::clamp(L->active, 0, (int)_panes.size() - 1)];

    NSString* msg = TrF(StringId::StatusLayoutApplied, {name});
    if (missing) msg = [msg stringByAppendingString:
                        TrF(StringId::MacMainWindowLayoutMissingChannelsSuffix,
                            {[NSString stringWithFormat:@"%d", missing], missing == 1 ? @"" : @"s"})];
    [self setStatus:msg];
}

// Delete a saved layout: drop it from the index and blank its value (the K/V store has no
// delete; an empty value reads as gone — same convention as Win32).
- (void)deleteLayoutClicked:(NSMenuItem*)sender {
    if (!_db) return;
    NSString* name = sender.representedObject;
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = TrF(StringId::MacMainWindowDeleteLayoutConfirm, {name});
    a.informativeText = Tr(StringId::MacMainWindowDeleteLayoutBody);
    [a addButtonWithTitle:Tr(StringId::ButtonDelete)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;
        const std::wstring wname = ws(name);
        auto names = [self savedLayoutNames];
        names.erase(std::remove(names.begin(), names.end(), wname), names.end());
        [self storeLayoutNames:names];
        _db->setSetting(L"layout_saved_" + wname, L"");
        [self setStatus:TrF(StringId::StatusLayoutDeleted, {name})];
    }];
}

// Create the non-invasive spectrum tap (idempotent; only when enabled). The one-time
// audio-capture consent prompt happens on the FIRST creation. macOS caches a denial,
// so a denied prompt won't re-ask — which is exactly why -updateSpectrumAvailability:
// must detect denial behaviourally rather than trust a non-nil tap.
- (void)startSpectrumTap {
    const int s = (int)MeterKind::Spectrum;
    if (_spectrumTap || !_meterCfg[s].enabled) return;
    _spectrumSilentTicks = 0;
    MeterView* view = _meters[s];  // capture the VIEW, not self (no retain cycle)
    _spectrumTap = [[SpectrumTap alloc] initWithBandCount:24
                                          spectrumHandler:^(const float* bands, int count) {
        [view pushSpectrum:bands count:count];  // RT audio thread → brief lock in the view
    }
                                             levelHandler:^(float db) {
        [view pushLevel:programmeDbfs(db, g_tapVolume.load(std::memory_order_relaxed))];  // the VU needle
    }];
    if (!_spectrumTap) [_meters[s] setAvailable:NO];  // macOS < 14.2, or the tap couldn't be built
}

- (void)stopSpectrumTap {
    [_spectrumTap stop];     // tears down the tap / aggregate / IOProc (AudioDeviceStop is synchronous)
    [_spectrumTap release];  // MRC: balance the +1 from -alloc. Safe — stop() already ran, so no IOProc
    _spectrumTap = nil;      // is in flight, and the view it captured is app-lifetime.
    _spectrumSilentTicks = 0;
    [_meters[(int)MeterKind::Spectrum] reset];
}

// Decide live-spectrum vs the "grant permission" placeholder by cross-checking the tap
// against libVLC's own view of the stream: audio flowing + audible but the tap silent
// for ~2s => capture denied/unavailable. This is the fix for the dark-strip failure
// that pulled the meter before (a denied tap "succeeds" but delivers only silence).
- (void)updateSpectrumAvailability:(const FlowStats&)fs {
    const int s = (int)MeterKind::Spectrum;
    if (!_meterCfg[s].enabled || !_spectrumTap) return;
    if ([_meters[s] consumeHadEnergy]) {         // the tap delivered real audio
        _spectrumEverHadEnergy = YES;            // => capture is granted for this session
        _spectrumSilentTicks = 0;
        [_meters[s] setAvailable:YES];
        return;
    }
    // Once the tap has EVER produced energy this session, capture is granted — a later
    // silent stretch is just quiet audio (a pause, a silent slate), NOT a permission
    // problem, so we must never flip to the placeholder. Only a tap that has never once
    // produced energy while audio is audible indicates a denied prompt.
    if (_spectrumEverHadEnergy) return;
    // Only suspect a denied tap when audio SHOULD be audible: an audio track is present,
    // data is flowing, and we're not muted. A video-only / no-audio / muted stream is
    // legitimately silent — don't nag. Needs a sustained silence so a quiet intro on an
    // audio stream doesn't false-flag.
    const BOOL audible = _player->hasAudioTrack() && fs.demuxBytesPerSec > 0 && _volume.doubleValue > 0;
    // Accumulate only AUDIBLE, tap-silent polls — and do NOT reset on a transient non-audible
    // one. demuxBytesPerSec legitimately reads 0 between HLS segments, so the old "32
    // CONSECUTIVE audible ticks" rule was perpetually reset by normal streaming and the
    // placeholder could never appear on a real stream (a denied tap looked like a dead meter).
    // The count is now cumulative: ~10s of genuinely audible playback with zero tap energy.
    // It is cleared only when the tap produces energy, or when the tap is (re)started/stopped.
    if (!audible) return;
    if (++_spectrumSilentTicks >= 40) [_meters[s] setAvailable:NO];
}

// Settings pull-down — the mac peer of the Win32 gear menu, laid out in the SAME structure
// (Channels ▸ / Recording ▸ / View ▸ / Layout ▸ / Language ▸, in Win32's order). Win32-only
// items with no mac equivalent are omitted: Theme ▸ (mac uses the native system appearance) and
// wake-to-record (a non-root mac app cannot arm a system wake).
- (void)showSettings:(NSButton*)sender {
    NSMenu* m = [[NSMenu alloc] init];

    // Playlists (Win32: "Open File…" at the top; mac adds playlist management).
    [[m addItemWithTitle:Tr(StringId::MenuOpenPlaylistFile) action:@selector(openFile:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MenuManagePlaylists) action:@selector(showPlaylists:) keyEquivalent:@""] setTarget:self];

    // Channels ▸ — favourites import/export + Categories include-filter + Hide-unavailable
    // + Resume-last-channel (Win32 parity).
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* chanItem = [m addItemWithTitle:Tr(StringId::MenuChannels) action:nil keyEquivalent:@""];
    NSMenu* chan = [[NSMenu alloc] init];
    // ⚠ autoenablesItems defaults to YES, and under it AppKit RECOMPUTES every item's enabled state
    // from NSMenuValidation just before display — silently discarding any explicit `enabled` set
    // below. This USED to live inside the Xtream branch further down, which meant the explicit flag
    // only survived for users whose playlist carried credentials: on a plain .m3u library the
    // dead-link item relabelled to "Checking for dead links…" but stayed clickable and silently
    // no-opped. Set it once for the whole submenu. Every other item here has a live target and
    // NSMenuItem.enabled defaults to YES, so none of them change behaviour.
    chan.autoenablesItems = NO;
    [[chan addItemWithTitle:Tr(StringId::MenuImportFavourites) action:@selector(importFavourites:) keyEquivalent:@""] setTarget:self];
    [[chan addItemWithTitle:Tr(StringId::MenuExportFavourites) action:@selector(exportFavourites:) keyEquivalent:@""] setTarget:self];
    [chan addItem:[NSMenuItem separatorItem]];
    [[chan addItemWithTitle:Tr(StringId::MenuCategories) action:@selector(showCategories:) keyEquivalent:@""] setTarget:self];
    // Hide unavailable channels (checkbox) — drop rows marked Dead (Win32 parity).
    NSMenuItem* hide = [chan addItemWithTitle:Tr(StringId::MenuHideUnavailable) action:@selector(toggleHideDead:) keyEquivalent:@""];
    hide.target = self;
    hide.state = _hideDead ? NSControlStateValueOn : NSControlStateValueOff;
    // The way BACK from a wrong dead-status. mac demotes a channel to Dead purely from playback
    // failure, and with "Hide unavailable" on those rows leave the grid — so without this the user
    // has no route to a channel the app misjudged (a provider blip, a geo-block, a flaky open).
    // The sweep itself, above the way back from it. Labelled with its own running state (the gear
    // menu is rebuilt on every open, so this needs no separate refresh) and disabled while EITHER
    // provider-facing worker holds the line.
    NSMenuItem* sweep = [chan addItemWithTitle:Tr(_sweepRunning ? StringId::MenuDeadLinkChecking
                                                               : StringId::MenuDeadLinkCheck)
                                        action:@selector(checkDeadLinks:)
                                 keyEquivalent:@""];
    sweep.target = self;
    sweep.enabled = !_sweepRunning && !_vodSyncRunning;
    [[chan addItemWithTitle:Tr(StringId::MenuDeadLinkClear) action:@selector(clearDeadStatuses:) keyEquivalent:@""] setTarget:self];
    // 🎬 Sync movies — present ONLY when a playlist actually carries Xtream credentials. A plain
    // .m3u has no player_api.php behind it, so for those users this item could never do anything
    // but fail, and an action that only ever fails is worse than an absent one. (The gear menu is
    // rebuilt on every open, so this tracks a playlist being added or removed with no extra work.)
    if (_db && !rabbitears::mac::xtreamTargets(*_db).empty()) {
        // (autoenablesItems is now turned off once for the whole submenu where `chan` is created —
        // it was here, which left every non-Xtream library with auto-enabling still on.)
        NSMenuItem* vod = [chan addItemWithTitle:Tr(_vodSyncRunning ? StringId::MenuVodSyncing
                                                                    : StringId::MenuVodSync)
                                          action:@selector(syncMovies:)
                                   keyEquivalent:@""];
        vod.target = self;
        vod.enabled = !_vodSyncRunning && !_sweepRunning;   // one provider-facing worker at a time
    }
    // Resume last channel (checkbox) — auto-play the last-watched channel on launch (Win32 parity).
    // Built fresh on each gear open, so the ✓ tracks _resumeLast without an explicit rebuild.
    NSMenuItem* resume = [chan addItemWithTitle:Tr(StringId::MenuResumeLastChannel) action:@selector(toggleResumeLast:) keyEquivalent:@""];
    resume.target = self;
    resume.state = _resumeLast ? NSControlStateValueOn : NSControlStateValueOff;
    chanItem.submenu = chan;

    // TV Guide + Refresh + Recording ▸.
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:Tr(StringId::TvGuideTitle) action:@selector(showGuide:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::MenuRefreshGuide) action:@selector(refreshGuide:) keyEquivalent:@""] setTarget:self];

    NSMenuItem* recItem = [m addItemWithTitle:Tr(StringId::MenuRecording) action:nil keyEquivalent:@""];
    NSMenu* recMenu = [[NSMenu alloc] init];
    [[recMenu addItemWithTitle:Tr(StringId::MacMainWindowRecordingsMenuItem) action:@selector(showRecordings:) keyEquivalent:@""] setTarget:self];
    [recMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* fmtItem = [recMenu addItemWithTitle:Tr(StringId::MenuRecordingFormat) action:nil keyEquivalent:@""];
    NSMenu* fmt = [[NSMenu alloc] init];  // Recording format ▸ (ts crash-safe default / mp4)
    struct { NSString* title; const wchar_t* v; } fmts[] = {
        { Tr(StringId::MacMainWindowFormatTsSafest), L"ts" }, { Tr(StringId::MenuFormatMp4), L"mp4" } };
    for (auto& f : fmts) {
        NSMenuItem* it = [fmt addItemWithTitle:f.title action:@selector(setRecFormat:) keyEquivalent:@""];
        it.target = self;
        it.representedObject = ns(f.v);
        it.state = (_recFormat == f.v) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    fmtItem.submenu = fmt;
    recItem.submenu = recMenu;

    // View ▸ — Single / Split / PiP + Video only (Win32 groups the view modes here).
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* viewItem = [m addItemWithTitle:Tr(StringId::MenuView) action:nil keyEquivalent:@""];
    NSMenu* view = [[NSMenu alloc] init];
    NSMenuItem* vSingle = [view addItemWithTitle:Tr(StringId::MenuSingleView) action:@selector(setViewSingle:) keyEquivalent:@""];
    NSMenuItem* vSplit  = [view addItemWithTitle:Tr(StringId::MenuSplitView) action:@selector(setViewSplit:) keyEquivalent:@""];
    NSMenuItem* vPip    = [view addItemWithTitle:Tr(StringId::MenuPictureInPicture) action:@selector(setViewPip:) keyEquivalent:@""];
    for (NSMenuItem* it in @[ vSingle, vSplit, vPip ]) it.target = self;
    vSingle.state = (_viewMode == ViewMode::Single) ? NSControlStateValueOn : NSControlStateValueOff;
    vSplit.state  = (_viewMode == ViewMode::Split)  ? NSControlStateValueOn : NSControlStateValueOff;
    vPip.state    = (_viewMode == ViewMode::Pip)    ? NSControlStateValueOn : NSControlStateValueOff;
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem* vOnly = [view addItemWithTitle:Tr(StringId::MenuVideoOnlyPlain) action:@selector(toggleVideoOnly:) keyEquivalent:@""];
    vOnly.target = NSApp.delegate;  // the AppDelegate owns this toggle (also the View menu-bar item)
    vOnly.state = self.videoOnly ? NSControlStateValueOn : NSControlStateValueOff;
    viewItem.submenu = view;

    // Layout ▸ — save / apply / delete the named multi-view arrangements.
    NSMenuItem* layoutsItem = [m addItemWithTitle:Tr(StringId::MenuLayout) action:nil keyEquivalent:@""];
    NSMenu* layouts = [[NSMenu alloc] init];
    [[layouts addItemWithTitle:Tr(StringId::MacMainWindowSaveCurrentLayout) action:@selector(saveLayout:) keyEquivalent:@""] setTarget:self];
    const auto names = [self savedLayoutNames];
    [layouts addItem:[NSMenuItem separatorItem]];
    if (names.empty()) {
        [layouts addItemWithTitle:Tr(StringId::MacMainWindowNoSavedLayouts) action:nil keyEquivalent:@""].enabled = NO;
    } else {
        for (const auto& n : names) {
            NSString* name = ns(n);
            NSMenuItem* it = [layouts addItemWithTitle:name action:@selector(applyLayoutClicked:) keyEquivalent:@""];
            it.target = self;
            it.representedObject = name;
        }
        NSMenuItem* delItem = [layouts addItemWithTitle:Tr(StringId::ButtonDelete) action:nil keyEquivalent:@""];
        NSMenu* del = [[NSMenu alloc] init];
        for (const auto& n : names) {
            NSString* name = ns(n);
            NSMenuItem* it = [del addItemWithTitle:name action:@selector(deleteLayoutClicked:) keyEquivalent:@""];
            it.target = self;
            it.representedObject = name;
        }
        delItem.submenu = del;
    }
    layoutsItem.submenu = layouts;

    // Meters ▸ (Win32's submenu): the size presets, the labels, the bridge window, then the setup dialog.
    // (Win32's "Or drag the strip's top edge" hint is a strip item mac lacks; here the floating bar's top
    // edge is the drag.)
    {
        NSMenuItem* metersItem = [m addItemWithTitle:Tr(StringId::MenuMetersSubmenu) action:nil keyEquivalent:@""];
        NSMenu* mm = [[[NSMenu alloc] init] autorelease];  // MRC: the item retains its submenu
        struct { StringId sid; int dp; } sizes[] = {
            {StringId::MenuMeterSizeStandard, rabbitears::mac::kMeterHeightStd},
            {StringId::MenuMeterSizeLarge, rabbitears::mac::kMeterHeightLarge},
            {StringId::MenuMeterSizeXLarge, rabbitears::mac::kMeterHeightXLarge}};
        for (const auto& sz : sizes) {
            NSMenuItem* it = [mm addItemWithTitle:Tr(sz.sid) action:@selector(selectMeterSize:) keyEquivalent:@""];
            it.target = self;
            it.tag = sz.dp;
            it.state = _meterHeightDp == sz.dp ? NSControlStateValueOn : NSControlStateValueOff;
        }
        [mm addItem:[NSMenuItem separatorItem]];
        NSMenuItem* lab = [mm addItemWithTitle:Tr(StringId::MenuMeterLabels) action:@selector(toggleMeterLabels:) keyEquivalent:@""];
        lab.target = self;
        lab.state = _meterLabels ? NSControlStateValueOn : NSControlStateValueOff;
        NSMenuItem* dots = [mm addItemWithTitle:Tr(StringId::MenuMeterTankDots) action:@selector(toggleTankDots:) keyEquivalent:@""];
        dots.target = self;
        dots.state = REDataFlowScaledDots() ? NSControlStateValueOn : NSControlStateValueOff;
        NSMenuItem* br = [mm addItemWithTitle:Tr(StringId::MenuMeterBridge) action:@selector(toggleMeterBridge:) keyEquivalent:@""];
        br.target = self;
        br.state = (_meterBridge && [_meterBridge isVisible]) ? NSControlStateValueOn : NSControlStateValueOff;
        [mm addItem:[NSMenuItem separatorItem]];
        [[mm addItemWithTitle:Tr(StringId::MenuMeterSetup) action:@selector(showMeters:) keyEquivalent:@""] setTarget:self];
        metersItem.submenu = mm;
    }

    // Language ▸ (mirrors the Win32 gear; the AppDelegate owns the selection + live apply). Built
    // fresh on each gear open, so the ✓ tracks the active language without an explicit rebuild.
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* langItem = [m addItemWithTitle:Tr(StringId::MenuLanguage) action:nil keyEquivalent:@""];
    NSMenu* langMenu = [[NSMenu alloc] init];
    struct { NSString* code; StringId sid; } langs[] = {
        { @"system", StringId::LangSystemDefault }, { @"en", StringId::LangEnglish },
        { @"ja", StringId::LangJapanese }, { @"zh-Hant", StringId::LangTraditionalChinese },
        { @"zh-HK", StringId::LangTraditionalChineseHK } };
    NSString* curLang = currentLanguagePref();
    for (auto& L : langs) {
        NSMenuItem* it = [langMenu addItemWithTitle:Tr(L.sid) action:@selector(selectLanguage:) keyEquivalent:@""];
        it.target = NSApp.delegate;
        it.representedObject = L.code;
        it.state = [curLang isEqualToString:L.code] ? NSControlStateValueOn : NSControlStateValueOff;
    }
    langItem.submenu = langMenu;

    // Logging ▸ — the mac peer of Win32's Settings ▸ System page. Only the LOG LEVEL is carried
    // over: the other half of that page is the beta-feature switchboard, and FeatureFlags.h
    // declares its enum with NO enumerators, so porting it would render an empty box.
    //
    // A submenu of radio items rather than a dialog, because mac has no settings-with-pages window
    // and this is exactly how Language already works here. Zero new catalog strings — the level
    // names, the section title and "Open log folder" are all shared ids Win32 already ships.
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* logItem = [m addItemWithTitle:Tr(StringId::SystemLoggingSection) action:nil keyEquivalent:@""];
    // Autoreleased, unlike its siblings above: the gear tree is a known pre-existing leak that the
    // handover deliberately leaves alone (a wrong release there is an over-release CRASH, not a
    // leak), so this fixes nothing around it but does not add to it either.
    NSMenu* logMenu = [[[NSMenu alloc] init] autorelease];
    const struct { diag::Level lv; StringId sid; } levels[] = {
        { diag::Level::Error, StringId::LogLevelError }, { diag::Level::Warn,  StringId::LogLevelWarn },
        { diag::Level::Info,  StringId::LogLevelInfo },  { diag::Level::Debug, StringId::LogLevelDebug },
        { diag::Level::Trace, StringId::LogLevelTrace } };
    const diag::Level curLevel = diag::level();
    for (auto& L : levels) {
        NSMenuItem* it = [logMenu addItemWithTitle:Tr(L.sid) action:@selector(selectLogLevel:) keyEquivalent:@""];
        it.target = self;
        it.tag = static_cast<NSInteger>(L.lv);   // scalar, so nothing to own (cf. the language menu)
        it.state = (L.lv == curLevel) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [logMenu addItem:[NSMenuItem separatorItem]];
    [[logMenu addItemWithTitle:Tr(StringId::SystemOpenLogFolder) action:@selector(openLogFolder:) keyEquivalent:@""] setTarget:self];
    logItem.submenu = logMenu;

    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:Tr(StringId::AboutCheckForUpdatesButton) action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:Tr(StringId::AboutWindowTitle) action:@selector(showAbout:) keyEquivalent:@""] setTarget:self];
    [m popUpMenuPositioningItem:nil
                     atLocation:NSMakePoint(0, NSHeight(sender.bounds) + 4)
                         inView:sender];
}

- (void)setRecFormat:(NSMenuItem*)sender {
    _recFormat = ws(sender.representedObject);
    if (_db) _db->setSetting(L"rec_format", _recFormat);
    [self setStatus:TrF(StringId::MacMainWindowRecordingFormatStatus, {sender.representedObject})];
}

// Settings ⚙ ▸ Recordings… — the schedule/rules manager window.
- (void)showRecordings:(id)__unused sender {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailable)]; return; }
    if (!_recordingsWC) _recordingsWC = [[RecordingsWindowController alloc] initWithDatabase:_db.get()
                                                                     statusWriter:_statusWriter.get()];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    // Cancel on a row being recorded: only the scheduler can stop its recorder (Win32 cb.cancel).
    ((RecordingsWindowController*)_recordingsWC).onStopRecording = ^(long long sid) {
        [me stopScheduleIfActive:sid];
    };
    [(RecordingsWindowController*)_recordingsWC presentRelativeTo:_window onChange:^{
        [me expandRecordingRules:YES];  // a re-enabled rule should re-queue now
        [me schedulerTick];
    }];
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

// Settings ▾ ▸ Meters… — the config dialog (peer of Win32 Settings → Meters).
- (void)showMeters:(id)__unused sender {
    MetersDialog* dlg = [[MetersDialog alloc] initWithDatabase:_db.get()];
    MainWindowController* __unsafe_unretained me = self;
    dlg.onGlassChanged = ^{  // the live glass preview reaches the real meters (and the bridge's) too
        for (int k = 0; k < 4; ++k) [me->_meters[k] setNeedsDisplay:YES];
        [me->_tank setNeedsDisplay:YES];
        [me->_meterBridge applyConfigs:me->_meterCfg labels:me->_meterLabels tank:me->_dataFlowOn];
    };
    [dlg presentForWindow:_window onApply:^{
        [self loadMeterConfig];
        [self applyMeterConfig];
        [self setStatus:Tr(StringId::MacMainWindowMeterSettingsSaved)];
    }];
    [dlg release];  // MRC: the sheet's completion handler keeps it alive until it closes
}

// Settings ▾ ▸ Manage Playlists… — enable/disable/delete imported playlists.
- (void)showPlaylists:(id)__unused sender {
    PlaylistsDialog* dlg = [[PlaylistsDialog alloc] initWithDatabase:_db.get()];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    dlg.onRefreshGuide = ^{ [me refreshGuide:nil]; };
    [dlg presentForWindow:_window onChange:^{ [self reloadAfterPlaylistChange]; }];
    [dlg release];  // MRC: the sheet's completion handler keeps it alive until it closes
}

// Settings ⚙ ▸ Channels ▸ Categories… — a checklist of group-titles; the grid then shows only the
// checked ones (empty = show all). Persisted in "category_filter" and applied by -refreshChannels.
- (void)showCategories:(id)__unused sender {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailable)]; return; }
    NSMutableArray<NSString*>* groups = [NSMutableArray array];
    for (const auto& g : _db->listGroups()) [groups addObject:ns(g)];
    NSMutableSet<NSString*>* sel = [NSMutableSet set];
    for (const auto& g : _categoryFilter) [sel addObject:ns(g)];
    CategoriesDialog* dlg = [[CategoriesDialog alloc] initWithGroups:groups selected:sel];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    [dlg presentForWindow:_window onApply:^(NSSet<NSString*>* selected) {
        me->_categoryFilter.clear();
        for (NSString* g in selected) me->_categoryFilter.insert(ws(g));
        [me persistCategoryFilter];
        [me refreshChannels];
    }];
    [dlg release];  // MRC: the sheet's completion handler keeps it alive until it closes
}

- (void)persistCategoryFilter {
    if (!_db) return;
    std::wstring joined;
    for (const auto& g : _categoryFilter) { if (!joined.empty()) joined += L'\n'; joined += g; }
    _db->setSetting(L"category_filter", joined);
}

// Re-sync the grid after Manage Playlists enables/disables/deletes something. If the
// active playlist is now gone or disabled, fall back to the last still-enabled one
// (or the empty state); otherwise just rebuild the filters + reload counts.
- (void)reloadAfterPlaylistChange {
    [self refreshArchiveMarkers];  // a deleted playlist takes its channels' flags with it
    if (!_db) return;
    const auto playlists = _db->listPlaylists();
    bool currentOK = false;
    long long lastEnabled = 0;
    for (const auto& p : playlists) {
        if (p.enabled) lastEnabled = p.id;                       // last enabled (added_at order)
        if (p.id == _currentPid && p.enabled) currentOK = true;
    }
    if (!currentOK) _currentPid = lastEnabled;  // 0 => all-channels view (empty if none enabled)

    // rebuildFilterMenu resets the popup to "All channels" (the enabled set can add or
    // remove groups/countries). If the active playlist is unchanged, preserve the user's
    // current filter across the rebuild — re-select it if it still exists, else fall back
    // to All. When the playlist itself changed, a fresh view legitimately starts at All.
    if (currentOK) [self rebuildFilterMenuPreservingSelection];
    else           [self rebuildFilterMenu];  // the playlist changed — a fresh view starts at All
    [self refreshChannels];
    if (!_currentPid && lastEnabled == 0 && !playlists.empty())
        [self setStatus:Tr(StringId::MacMainWindowAllPlaylistsDisabledSee)];
}

// ---- TV Guide (EPG) ----------------------------------------------------------
// Peer of the Win32 onEpgRefresh/onEpgDone (MainWindowCommands.cpp): download + gunzip
// + parse the XMLTV guide for every enabled playlist that carries a guide URL, off the
// main queue, then store the parsed programmes on the main thread. Newest refresh wins.

// ---- Refresh Guide --------------------------------------------------------------------------
// The mac peer of Win32's onRefreshGuide + EpgStore + finishEpgRefresh (MainWindowCommands.cpp,
// ui/EpgStore.cpp). ONE worker does everything heavy, in order, and the main thread keeps only
// what needs the controller: the recording-rule pass and the result.
//  1. Fetch — per playlist, its guide link split into its guides (core/GuideSources.h: an M3U
//     header's x-tvg-url may list several); each is downloaded with a running byte count and
//     parsed with a running programme count (status-bar progress, at most one line per 250 ms),
//     then the guides are MERGED (mergeGuideSources: a channel's programmes from the first guide
//     listing it). A guide that fails while the others load is named in the results.
//  2. Store — on the worker's OWN connection (opened with upgradeSchema=false; the app's own
//     already migrated), one short transaction per playlist with a pause between them, so a
//     main-thread write waiting on the lock gets it (the scheduler's status writes are retried —
//     ScheduleStatusWriter — most others are not). It used to run on the main thread.
//  3. Index — the TV Guide search's index is rebuilt ONCE, after every playlist (EPG_SEARCH.md §3).
// One refresh at a time (the second click says so); a quit cancels between transactions.


- (void)refreshGuide:(id)__unused sender {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailable)]; return; }
    if (_epgRefreshing) { [self setStatus:Tr(StringId::StatusBusyWait)]; return; }
    struct Target { long long id; std::wstring name; std::wstring url; };
    auto targets = std::make_shared<std::vector<Target>>();
    for (const auto& pl : _db->listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets->push_back({pl.id, pl.name, pl.epgUrl});
    if (targets->empty()) {
        [self showResults:Tr(StringId::RefreshGuideNoSourceHeading)
                     info:Tr(StringId::MacMainWindowNoGuideSourceBody)];
        return;
    }
    _epgRefreshing = YES;
    g_epgCancel = false;
    [self setStatus:Tr(StringId::LoadingContactingGuide)];
    diag::info(L"EPG refresh start: " + std::to_wstring(targets->size()) + L" playlist(s)");
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    const std::wstring dbPath = Database::defaultDbPath();
    std::vector<rabbitears::mac::GuideTarget> work;
    for (const auto& t : *targets) work.push_back({t.id, t.name, t.url});
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        // Progress lines go to the status bar. The running counts arrive far faster than anyone can
        // read them, so at most one per 250 ms; a skipped line is overtaken by the next. Step names
        // (`force`) always go. Called only from this worker or the download's delegate queue while
        // this worker waits on it, so `lastPost` is never touched concurrently.
        using Clock = std::chrono::steady_clock;
        Clock::time_point lastPost = Clock::now() - std::chrono::seconds(1);
        auto post = [&lastPost, me](const std::wstring& line, bool force) {
            const Clock::time_point now = Clock::now();
            if (!force && now - lastPost < std::chrono::milliseconds(250)) return;
            lastPost = now;
            NSString* text = [ns(line) retain];  // MRC: the main-queue block outlives this pool
            dispatch_async(dispatch_get_main_queue(), ^{
                [me setStatus:text];
                [text release];
            });
        };
        auto res = std::make_shared<rabbitears::mac::GuideRefreshResult>(
            rabbitears::mac::refreshGuides(work, dbPath, post, g_epgCancel));
        dispatch_async(dispatch_get_main_queue(), ^{ [me finishGuideRefresh:res]; });
    });
}

// The worker is done: log each playlist's outcome, run the recording-rule pass over the new guide,
// and show the results. (Peer of Win32 finishEpgRefresh.)
- (void)finishGuideRefresh:(std::shared_ptr<rabbitears::mac::GuideRefreshResult>)res {
    _epgRefreshing = NO;
    if (g_epgCancel || !_db) return;  // quitting
    int okCount = 0, totalProg = 0;
    NSMutableString* detail = [NSMutableString string];
    for (auto& f : res->fetches) {
        NSMutableString* guideLines = [NSMutableString string];
        for (const auto& w : f.sourceErrors) {
            [guideLines appendString:TrF(StringId::MacMainWindowEpgErrorLine, {ns(f.name), ns(w)})];
            diag::warn(L"EPG refresh for \"" + f.name + L"\": " + w);
        }
        for (const auto& l : f.sourceLog) diag::info(L"EPG \"" + f.name + L"\" " + l);
        if (!f.error.empty()) {
            [detail appendString:TrF(StringId::MacMainWindowEpgErrorLine, {ns(f.name), ns(f.error)})];
            [detail appendString:guideLines];
            diag::error(L"EPG refresh failed for \"" + f.name + L"\": " + f.error + L" (download took " +
                        std::to_wstring(f.downloadMs) + L" ms)");
            continue;
        }
        ++okCount;
        totalProg += f.stored;
        [detail appendString:TrF(StringId::EpgDetailProgrammesLine,
                                 {ns(f.name), [NSString stringWithFormat:@"%d", f.stored]})];
        [detail appendString:guideLines];
        diag::info(L"EPG stored " + std::to_wstring(f.stored) + L" programmes for \"" + f.name +
                   L"\" (download " + std::to_wstring(f.downloadMs) + L" ms, parse " +
                   std::to_wstring(f.parseMs) + L" ms, " + std::to_wstring(f.parsed) + L" parsed, store " +
                   std::to_wstring(f.storeMs) + L" ms)");
    }
    if (res->index != rabbitears::mac::GuideRefreshResult::Index::NotRun)
        diag::info(L"EPG search index rebuild: " + std::to_wstring(res->indexMs) + L" ms" +
                   (res->index == rabbitears::mac::GuideRefreshResult::Index::Rebuilt
                        ? L""
                        : L" — FAILED (the next search session rebuilds it): " + res->indexError));
    NSString* summary = okCount > 0
        ? TrF(StringId::EpgStoredSummary,
              {[NSString stringWithFormat:@"%d", totalProg], [NSString stringWithFormat:@"%lu", (unsigned long)res->channels]})
        : Tr(StringId::EpgRefreshFailedSummary);
    if (okCount > 0) {  // a fresh guide is exactly when a series rule learns about next week's airings
        [self setStatus:Tr(StringId::LoadingCheckingRules)];
        const int queued = [self expandRecordingRules:YES];
        if (queued > 0) {
            [detail appendString:TrF(StringId::EpgRulesQueuedDetail, {[NSString stringWithFormat:@"%d", queued]})];
            summary = [summary stringByAppendingString:TrF(StringId::EpgQueuedByRulesSummary,
                                                           {[NSString stringWithFormat:@"%d", queued]})];
        }
        [(TvGuideWindowController*)_guideWC guideDidChange];  // an open guide shows the new programmes
    }
    [self setStatus:summary];
    [self showResults:summary info:detail];
}


// ---- catch-up (Xtream archives — the mac peer of Win32 playCatchup / refreshArchiveMarkers) ------

// The provider sync wrote new catch-up flags (or a playlist changed): re-read them for the grid's ↺
// markers and -playCatchup:, and let an open TV Guide take its new archive picks in place.
- (void)refreshArchiveMarkers {
    if (!_db) return;
    auto fresh = _db->channelArchiveDays();
    const bool changed = fresh != _archiveDays;
    _archiveDays = std::move(fresh);
    if (!changed) return;
    [_table reloadData];  // the ↺ after a name (cells only — the rows and selection stay)
    [(TvGuideWindowController*)_guideWC archiveDidChange];
}

// Play `title` (startUtc..stopUtc) out of `archiveChannel`'s archive in the active pane. NO, with an
// explanation, when it cannot even try. The URL is never logged here; the player's own line has it,
// masked by the log's redaction (the /timeshift/USER/PASS/ shape).
- (BOOL)playCatchup:(long long)archiveChannel title:(NSString*)title start:(long long)startUtc stop:(long long)stopUtc {
    auto explain = [&](NSString* why) {
        diag::warn(L"catch-up refused: " + ws(why));
        [self showResults:Tr(StringId::CatchupUnavailableHeading) info:why];
    };
    const std::optional<Channel> ch = _db ? _db->channelById(archiveChannel) : std::nullopt;  // enabled playlists
    const auto daysIt = _archiveDays.find(archiveChannel);
    if (!ch || daysIt == _archiveDays.end()) { explain(Tr(StringId::CatchupNoArchive)); return NO; }
    const long long now = (long long)time(nullptr);
    if (startUtc >= now) { explain(Tr(StringId::CatchupNotStarted)); return NO; }  // offered only once started
    if (startUtc < now - (long long)daysIt->second * 86400) {
        explain(TrF(StringId::CatchupTooOld, {ns(ch->name), [NSString stringWithFormat:@"%d", daysIt->second]}));
        return NO;
    }
    // The login from the playlist's own URL (as the movie sync's); the stream id from the channel's.
    rabbitears::XtreamCreds creds;
    bool haveCreds = false;
    for (const auto& pl : _db->listPlaylists())
        if (pl.id == ch->playlistId) haveCreds = rabbitears::parseXtreamPlaylistUrl(pl.sourceUrl, creds);
    const long long streamId = rabbitears::xtreamLiveStreamId(ch->streamUrl);
    if (!haveCreds || streamId <= 0) { explain(Tr(StringId::CatchupNoArchive)); return NO; }
    // The panel reads the start on ITS clock: the zone it named at the last sync, on the programme's
    // day (daylight saving); else the offset measured then; else UTC.
    const std::wstring pid = std::to_wstring(ch->playlistId);
    int offset = 0;
    const std::wstring zone = _db->getSetting(L"archive_tz_" + pid).value_or(L"");
    if (!rabbitears::utcOffsetAt(zone, startUtc, &offset))
        offset = (int)std::wcstol(_db->getSetting(L"archive_utc_offset_" + pid).value_or(L"0").c_str(), nullptr, 10);
    const int minutes = (int)std::max<long long>(1, (stopUtc - startUtc + 59) / 60);
    Channel c = *ch;
    c.id = 0;  // not a channel: no "last channel", no dead-link verdict
    c.streamUrl = rabbitears::xtreamTimeshiftUrl(creds, streamId, startUtc, minutes, offset);
    c.name = ws(TrF(StringId::CatchupNowPlayingName, {ns(ch->name), title}));
    if (c.streamUrl.empty()) { explain(Tr(StringId::CatchupNoArchive)); return NO; }
    diag::info(L"catch-up: #" + std::to_wstring(ch->id) + L" \"" + ch->name + L"\", " + std::to_wstring(minutes) +
               L" min from " + std::to_wstring(startUtc) + L" (server zone " + (zone.empty() ? L"?" : zone) +
               L", UTC" + (offset >= 0 ? L"+" : L"") + std::to_wstring(offset) + L" s)");
    [self playChannel:c intoPane:_activePane];
    if (MacVideoPane* ap = [self activePane]) {
        ap->catchup = true;
        ap->catchupPlayed = false;
    }
    return YES;
}

// Lazily create the guide window controller and wire its recording callbacks once. Both
// -showGuide: and -showInGuide: go through this so the Schedule/Record-Series actions on a
// programme are always connected.
- (TvGuideWindowController*)guideController {
    if (!_guideWC) {
        _guideWC = [[TvGuideWindowController alloc] initWithDatabase:_db.get()];
        MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
        TvGuideWindowController* g = _guideWC;
        g.onSchedule = ^(NSString* tvgId, NSString* name, NSString* title, long long a, long long b) {
            [me scheduleRecordingForTvgId:tvgId name:name title:title start:a stop:b];
        };
        g.onRecordSeries = ^(NSString* tvgId, NSString* name, NSString* title) {
            [me addSeriesRuleForTvgId:tvgId name:name title:title];
        };
        // A guide row's right-click favourite toggle (Win32 cb.isFavourite / onToggleFavourite).
        g.isFavourite = ^BOOL(NSString* tvgId) {
            if (!me->_db) return NO;
            const auto ch = me->_db->channelByTvgId(ws(tvgId));
            return ch && ch->favourite;
        };
        g.onToggleFavourite = ^(NSString* tvgId, NSString* name) {
            if (!me->_db) return;
            const auto ch = me->_db->channelByTvgId(ws(tvgId));
            if (!ch) return;
            me->_db->toggleFavourite(ch->id);  // ch->favourite is the pre-toggle state
            [me refreshChannels];               // the ★ column + a Favourites view stay correct
            [me setStatus:TrF(ch->favourite ? StringId::StatusRemovedFavourite : StringId::StatusAddedFavourite, {name})];
        };
        g.isGuideRefreshing = ^BOOL { return me->_epgRefreshing; };
        // Catch-up (experimental): play a programme from its channel's archive; YES hides the guide
        // so the programme is visible (Win32 cb.onPlayFromStart).
        g.onPlayFromStart = ^BOOL(long long arch, NSString* title, long long a, long long b) {
            if (![me playCatchup:arch title:title start:a stop:b]) return NO;
            [me->_window makeKeyAndOrderFront:nil];
            return YES;
        };
    }
    return _guideWC;
}

// Open (or re-reveal) the channels×time guide window. The window controller queries the
// DB and lays the grid out; a picked programme calls back to -playFromGuide:name:.
- (void)showGuide:(id)__unused sender {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailable)]; return; }
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    [[self guideController] presentRelativeTo:_window
                                      onPlay:^(NSString* tvgId, NSString* name) {
        [me playFromGuide:tvgId name:name];
    }];
}

// Resolve a guide row's tvg-id back to a playable channel and play it (peer of the Win32
// onEpgGuide onPlay callback); hide the guide so the picked channel is actually visible.
- (void)playFromGuide:(NSString*)tvgId name:(NSString*)name {
    if (!_db) return;
    std::optional<Channel> ch;
    if (tvgId.length) ch = _db->channelByTvgId(ws(tvgId));
    if (ch) {
        [self playChannel:*ch];
        [(TvGuideWindowController*)_guideWC hide];  // don't leave the show playing behind the guide
        [_window makeKeyAndOrderFront:nil];
    } else {
        [self showResults:Tr(StringId::GuideNoMatchHeading)
                     info:TrF(StringId::MacMainWindowGuideNoMatchBody, {name})];
    }
}

// Row action: open the TV Guide scrolled to (and highlighting) this channel's row. The guide
// only carries channels with stored programmes, so explain the two miss cases distinctly.
- (void)showInGuide:(id)__unused sender {
    const NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_channels.size() || !_db) return;
    const Channel& c = _channels[(size_t)row];
    if (c.tvgId.empty()) {
        [self showResults:Tr(StringId::MacMainWindowNotInGuideHeading)
                     info:TrF(StringId::MacMainWindowNoTvgIdBody, {ns(c.name)})];
        return;
    }
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    NSString* tvg = ns(c.tvgId);
    const REGuideShowResult r = [[self guideController] presentRelativeTo:_window
                                                             showChannel:tvg
                                                                  onPlay:^(NSString* t, NSString* n) {
        [me playFromGuide:t name:n];
    }];
    // On NoGuide the controller already alerted; only ChannelMissing needs our explanation.
    if (r == REGuideShowChannelMissing)
        [self showResults:Tr(StringId::MacMainWindowNotInGuideHeading)
                     info:TrF(StringId::MacMainWindowNotInGuideYetBody, {ns(c.name)})];
}

// Export every favourite to an Extended-M3U file (round-trips through importFavourites: and any
// IPTV player). Uses the shared writeM3u so the dialect matches exactly what the parser reads.
- (void)exportFavourites:(id)__unused sender {
    if (!_db) return;
    const std::vector<Channel> favs = _db->favourites();
    if (favs.empty()) {
        [self showResults:Tr(StringId::MacMainWindowNoFavouritesHeading) info:Tr(StringId::MacMainWindowExportNoFavesBody)];
        return;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = @"favourites.m3u";
    panel.allowedFileTypes = @[ @"m3u", @"m3u8" ];  // deprecated but matches openFile:; -Wdeprecated is benign
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSModalResponseOK || !panel.URL) return;
        rabbitears::M3uDocument doc;
        for (const auto& c : favs) {
            rabbitears::ParsedChannel pc;
            pc.name       = c.name;
            pc.streamUrl  = c.streamUrl;
            pc.logoUrl    = c.logoUrl;
            pc.groupTitle = c.groupTitle;
            pc.tvgId      = c.tvgId;
            pc.tvgName    = c.tvgName;
            pc.chno       = c.lcn.value_or(-1);  // export the custom LCN as tvg-chno when set
            pc.userAgent  = c.userAgent;
            pc.referrer   = c.referrer;
            doc.channels.push_back(std::move(pc));
        }
        const std::string bytes = rabbitears::writeM3u(doc);
        NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        NSError* err = nil;
        if ([data writeToURL:panel.URL options:NSDataWritingAtomic error:&err])
            [self showResults:Tr(StringId::MacMainWindowFavesExportedHeading)
                         info:TrF(StringId::MacMainWindowFavesExportedBody,
                               {[NSString stringWithFormat:@"%zu", favs.size()],
                                favs.size() == 1 ? @"" : @"s",
                                panel.URL.lastPathComponent})];
        else
            [self showResults:Tr(StringId::MacMainWindowExportFailedHeading)
                         info:(err.localizedDescription ?: Tr(StringId::MacMainWindowCouldntWriteFile))];
    }];
}

// Import favourites from an M3U: mark any channel already in the library whose stream URL (else
// tvg-id) matches an entry in the file. Does NOT add channels — favourites are a flag on existing
// rows, so importing into an empty/mismatched library is a no-op we report honestly.
- (void)importFavourites:(id)__unused sender {
    if (!_db) return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    panel.allowedFileTypes = @[ @"m3u", @"m3u8" ];
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSModalResponseOK || !panel.URL) return;
        std::wstring perr;
        rabbitears::M3uDocument doc = rabbitears::parseM3uFile(ws(panel.URL.path), &perr);
        if (doc.channels.empty()) {
            [self showResults:Tr(StringId::MacMainWindowNothingToImportHeading)
                         info:perr.empty() ? Tr(StringId::MacMainWindowFileNoChannels) : ns(perr)];
            return;
        }
        // Match by SETS, not last-wins maps: the same stream URL can appear on several channel
        // rows (a channel duplicated across playlists), and every one of them should become a
        // favourite — a map keyed on URL would keep only the last row and mark it arbitrarily.
        // ⚠ COMPARE CANONICAL URLs ON BOTH SIDES. Schema v9 rewrote every stored `stream_url` to
        // its canonical spelling (default port stripped), so a favourites .m3u exported by an
        // older build carries the literal `http://host:80/…` form that no longer matches the
        // library string-for-string. Matching raw would silently degrade every row to tvg-id-only
        // — lossy for any channel without a tvg-id, and reported as an innocent "unmatched" count
        // rather than as the bug it is. canonicalStreamUrl is idempotent (UrlCanon.h:49), so this
        // is equally correct against a pre-v9 library and against a file exported after v9.
        std::set<std::wstring> wantUrls, wantTvgs;   // exported (from the file)
        for (const auto& pc : doc.channels) {
            wantUrls.insert(rabbitears::canonicalStreamUrl(pc.streamUrl));
            if (!pc.tvgId.empty()) wantTvgs.insert(pc.tvgId);
        }
        std::set<std::wstring> libUrls, libTvgs;     // present (in the library)
        std::set<long long> toFav;
        for (const auto& c : _db->allChannels()) {
            const std::wstring cu = rabbitears::canonicalStreamUrl(c.streamUrl);
            libUrls.insert(cu);
            if (!c.tvgId.empty()) libTvgs.insert(c.tvgId);
            // Favourite this row if its URL was exported, else if its tvg-id was (URL preferred).
            if (wantUrls.count(cu) ||
                (!c.tvgId.empty() && wantTvgs.count(c.tvgId)))
                toFav.insert(c.id);
        }
        // An exported entry is "unmatched" when nothing in the library shares its URL or tvg-id.
        // Canonical here too — otherwise the fix would mirror the bug in the count the user sees.
        int unmatched = 0;
        for (const auto& pc : doc.channels)
            if (!libUrls.count(rabbitears::canonicalStreamUrl(pc.streamUrl)) &&
                !(!pc.tvgId.empty() && libTvgs.count(pc.tvgId)))
                ++unmatched;
        for (long long id : toFav) _db->setFavourite(id, true);
        [self refreshChannels];
        [self showResults:Tr(StringId::MacMainWindowFavesImportedHeading)
                     info:TrF(StringId::MacMainWindowFavesImportedBody,
                           {[NSString stringWithFormat:@"%zu", toFav.size()],
                            toFav.size() == 1 ? @"" : @"s",
                            unmatched ? TrF(StringId::MacMainWindowFavesUnmatchedSuffix,
                                            {[NSString stringWithFormat:@"%d", unmatched],
                                             unmatched == 1 ? @"y" : @"ies"})
                                      : @""})];
    }];
}

- (void)showAbout:(id)sender { [(AppDelegate*)NSApp.delegate showAboutPanel:sender]; }

- (void)playClicked:(id)__unused sender { [self playRow:_table.clickedRow]; }

// Double-click a row (or Return) plays it; a single click just selects.
- (void)playSelectedRow:(id)__unused sender {
    [self playRow:_table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow];
}

// ---- volume (bottom bar) ----

- (void)volumeChanged:(id)__unused sender {
    const int v = (int)_volume.doubleValue;
    _player->setVolume(v);
    g_tapVolume = std::clamp(v / 100.0f, 0.0f, 1.0f);
    if (v > 0) _preMuteVolume = v;
    _muteBtn.title = (v == 0) ? @"🔇" : @"🔊";
    if (_db) _db->setSetting(L"volume", std::to_wstring(v));
}

- (void)toggleMute:(id)__unused sender {
    _volume.doubleValue = (_volume.doubleValue == 0) ? _preMuteVolume : 0;
    [self volumeChanged:nil];
}

// Bottom-bar meter toggle — hide/show the whole meter line (independent of the
// per-kind Show checkboxes in Settings ▸ Meters…).
- (void)toggleMeters:(id)__unused sender {
    _metersHidden = !_metersHidden;
    if (_db) _db->setSetting(L"meters_hidden", _metersHidden ? L"1" : L"0");
    _meterBtn.alphaValue = _metersHidden ? 0.35 : 1.0;
    [self applyMeterLayout];
}

- (void)toggleFavouriteForRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size() || !_db) return;
    _db->toggleFavourite(_channels[(size_t)row].id);
    [self refreshChannels];  // re-query so the ★ column + a Favourites filter stay correct
}

- (void)toggleFavourite:(id)__unused sender {  // row context menu
    [self toggleFavouriteForRow:(_table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow)];
}

// Single-click in the ★ column toggles that row's favourite; a click in any other
// column just selects (double-click still plays, via doubleAction).
- (void)gridClicked:(id)__unused sender {
    const NSInteger row = _table.clickedRow, col = _table.clickedColumn;
    if (row < 0 || col < 0) return;
    if ([_table.tableColumns[(NSUInteger)col].identifier isEqualToString:@"fav"])
        [self toggleFavouriteForRow:row];
}

// Set/clear a channel's LCN (the # column) via a small prompt.
- (void)editChannelNumber:(id)__unused sender {
    const NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_channels.size() || !_db) return;
    const Channel& c = _channels[(size_t)row];
    const long long cid = c.id;  // capture by value; _channels can change before the sheet returns
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = TrF(StringId::MacMainWindowChannelNumberTitle, {ns(c.name)});
    a.informativeText = Tr(StringId::MacMainWindowChannelNumberPrompt);
    [a addButtonWithTitle:Tr(StringId::MacMainWindowSetButton)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 220, 24)];
    input.stringValue = c.lcn ? [NSString stringWithFormat:@"%d", *c.lcn] : @"";
    a.accessoryView = input;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;
        NSString* t = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        std::optional<int> lcn;
        if (t.length) lcn = (int)t.integerValue;
        self->_db->setChannelNumber(cid, lcn);
        [self refreshChannels];
    }];
}

- (void)playRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return;
    [self playChannel:_channels[(size_t)row]];
}

// Play `c` into a SPECIFIC pane. Only the active pane is audible and owns the title/status/
// meters/resume-state; a background pane (e.g. the PiP inset) starts muted. The 250ms tick
// re-asserts that mute once the audio track actually shows up.
- (void)playChannel:(const Channel&)c intoPane:(int)idx {
    if (idx < 0 || idx >= (int)_panes.size()) return;
    // 🔴 max_connections:1 — the sync gate cannot be start-only. -syncMovies: refuses while
    // anything is playing or recording, but nothing stops the user pressing Play two seconds into
    // a catalogue download, and on a one-connection line that is the sync kicking the stream the
    // user just asked for. The user's playback always wins, so the sync stands down. Soft: an
    // in-flight HTTP body still finishes (httpGet has no cancellation handle), but no further
    // request is issued and nothing more is written.
    rabbitears::mac::cancelVodSync();
    // Same rule, same reason: the sweep is the OTHER provider-facing worker, and its probes are
    // worse to leave running than the sync's downloads — a probe refused for connection capacity
    // reads as an unambiguous rejection and persists dead_status=Dead on a working channel.
    rabbitears::mac::cancelDeadLinkSweep();
    MacVideoPane* p = _panes[(size_t)idx].get();
    // New media in this pane: any pending seek target belongs to the OLD one. Unconditional —
    // a background pane costs nothing to clear, and the alternative (comparing against
    // _activePane) is one more place to get the scoping wrong.
    [self invalidateSeekLatch];
    p->player->play(c.streamUrl, c.userAgent, c.referrer);
    p->player->setVolume((int)_volume.doubleValue);  // libVLC resets volume per media
    p->player->setMuted(idx != _activePane);         // single-audio: only the active pane
    p->channel = c;
    p->channelId = c.id;
    p->everPlayed = false;  // new play attempt — reset the "was ever live" flag (dead-status heuristic)
    p->catchup = false;     // -playCatchup: sets it again after this call; any other play is a channel
    p->catchupPlayed = false;
    if (idx != _activePane) {
        [self setStatus:TrF(StringId::MacMainWindowPlayingInPane,
                         {Tr(_viewMode == ViewMode::Pip ? StringId::MacMainWindowPipShort
                                                        : StringId::MacMainWindowBackgroundPane),
                          ns(c.name)})];
        return;
    }
    if (_meterCfg[(int)MeterKind::Spectrum].enabled) [self startSpectrumTap];  // Spectrum is opt-in
    if (_db && c.id != 0)  // a catch-up programme is not a channel: nothing to resume
        _db->setSetting(L"last_channel_id", std::to_wstring(c.id));  // resume this on next launch
    _window.title = TrF(StringId::MacMainWindowTitleWithChannel, {ns(c.name)});
    [self setStatus:TrF(StringId::StatusPlaying, {ns(c.name)})];
}

// The shared "start playing this channel" path — used by the grid (playRow:) and the
// TV Guide (playFromGuide:name:). Always targets the active pane.
- (void)playChannel:(const Channel&)c { [self playChannel:c intoPane:_activePane]; }

// ---- NSTableView data source / delegate ----

- (NSInteger)numberOfRowsInTableView:(NSTableView*)__unused t { return (NSInteger)_channels.size(); }

- (id)tableView:(NSTableView*)__unused t
    objectValueForTableColumn:(NSTableColumn*)col
                          row:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return @"";
    const Channel& c = _channels[(size_t)row];
    NSString* id_ = col.identifier;
    if ([id_ isEqualToString:@"fav"])   return c.favourite ? @"★" : @"☆";  // both clickable to toggle
    if ([id_ isEqualToString:@"num"])   return c.lcn ? [NSString stringWithFormat:@"%d", *c.lcn] : @"";
    if ([id_ isEqualToString:@"logo"]) {
        // Kicks off an async load on a cache miss; the placeholder holds the cell meanwhile.
        NSImage* img = c.logoUrl.empty() ? nil : [_logoLoader imageForURL:ns(c.logoUrl)];
        return img ?: _logoPlaceholder;
    }
    if ([id_ isEqualToString:@"group"]) return ns(c.groupTitle);
    // A channel that keeps a catch-up archive: a ↺ after its name (Win32 ChannelGridControl).
    if (!_archiveDays.empty() && _archiveDays.count(c.id))
        return [ns(c.name) stringByAppendingString:@"  ↺"];
    return ns(c.name);  // "name"
}

// A logo finished loading — reload just the VISIBLE logo cells so the new image appears.
// Coalesced to one reload per runloop turn (many logos can arrive in a burst), and confined to
// the visible rows (loads are only ever kicked off for visible cells, and the loader is keyed
// by URL so a row that scrolled away is harmless). Main thread (LogoLoader hops here).
- (void)logosDidLoad {
    if (_logoReloadScheduled) return;
    _logoReloadScheduled = YES;
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime
    dispatch_async(dispatch_get_main_queue(), ^{
        me->_logoReloadScheduled = NO;
        const NSInteger col = [me->_table columnWithIdentifier:@"logo"];
        if (col < 0) return;
        const NSRange vis = [me->_table rowsInRect:me->_table.visibleRect];
        if (vis.length == 0) return;
        [me->_table reloadDataForRowIndexes:[NSIndexSet indexSetWithIndexesInRange:vis]
                              columnIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)col]];
    });
}

// Grey out unavailable (Dead) rows so they read as inactive even when not hidden. Cells are reused,
// so set the colour for EVERY row (reset non-dead/selected), not just the dead ones.
- (void)tableView:(NSTableView*)t willDisplayCell:(id)cell
   forTableColumn:(NSTableColumn*)__unused col row:(NSInteger)row {
    if (![cell respondsToSelector:@selector(setTextColor:)]) return;
    const BOOL dead = row >= 0 && row < (NSInteger)_channels.size()
                      && _channels[(size_t)row].deadStatus == DeadStatus::Dead;
    const BOOL selected = [t.selectedRowIndexes containsIndex:(NSUInteger)row];
    // An emphasized selection (table is first responder in the key window) draws WHITE text; an
    // unemphasized one draws dark text — only use the alternate (white) colour when emphasized.
    const BOOL emphasized = t.window.isKeyWindow && t.window.firstResponder == t;
    NSColor* color;
    if (dead && !selected) color = NSColor.tertiaryLabelColor;  // grey out unavailable rows
    else if (selected)     color = emphasized ? NSColor.alternateSelectedControlTextColor
                                              : NSColor.controlTextColor;
    else                   color = NSColor.controlTextColor;
    [cell setTextColor:color];  // cell is id + guarded by respondsToSelector: above
}

@end
