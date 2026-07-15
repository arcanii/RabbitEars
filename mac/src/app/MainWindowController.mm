// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#import <IOKit/pwr_mgt/IOPMLib.h>  // IOPMAssertion — keep-awake while recording

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/M3uWriter.h"
#include "core/RecordingRules.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "models/Programme.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
#include "ui/VideoGrid.h"  // computeVideoPanes + ViewMode — multi-view pane geometry
#include "platform/Log.h"
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
#import "RecordingsWindowController.h"
#import "SpectrumTap.h"
#import "TermsDialog.h"
#import "TvGuideWindowController.h"
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
@end

@implementation DraggableMeterBar {
    NSPoint _grab;  // cursor offset within the bar at mouse-down
}
- (void)mouseDown:(NSEvent*)e { _grab = [self convertPoint:e.locationInWindow fromView:nil]; }
- (void)mouseDragged:(NSEvent*)e {
    NSView* pane = self.superview;
    if (!pane) return;
    const NSPoint p = [pane convertPoint:e.locationInWindow fromView:nil];
    const CGFloat maxX = std::max(0.0, pane.bounds.size.width - self.frame.size.width);
    const CGFloat maxY = std::max(0.0, pane.bounds.size.height - self.frame.size.height);
    NSRect f = self.frame;
    f.origin.x = std::clamp(p.x - _grab.x, 0.0, maxX);
    f.origin.y = std::clamp(p.y - _grab.y, 0.0, maxY);
    self.frame = f;
}
- (void)mouseUp:(NSEvent*)__unused e { if (_onMoved) _onMoved(); }
@end

// One video pane in the multi-view area: a video surface (a child of the video container)
// + its libVLC player + what it's currently playing. Single view uses one pane; Split/2×2
// and PiP use several. The pane OWNS its player; its view is owned by the container (super).
struct MacVideoPane {
    NSView*                                view = nil;
    std::unique_ptr<rabbitears::VlcPlayerMac> player;
    rabbitears::Channel                    channel;      // currently playing (empty when idle)
    long long                              channelId = 0;
};

// Filter popup tags.
enum { kFilterAll = 0, kFilterFavourites = 1, kFilterGroup = 2, kFilterCountry = 3 };

// Chrome metrics, shared by showWindow and the hide/show-chrome relayout.
static const CGFloat kBarH = 46;     // top command bar height
static const CGFloat kStatusH = 22;  // bottom status/volume bar height
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

@implementation MainWindowController {
    NSWindow*      _window;
    NSSearchField* _search;
    NSPopUpButton* _filter;
    std::set<std::wstring> _categoryFilter;  // include-set of group-titles ("category_filter"); empty = show all
    RETableView*   _table;
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
    std::wstring   _recFormat;   // recording container: "ts" (default) / "mp4"
    BOOL           _resumeLast;  // auto-play the last channel on launch (setting "resume_last", default on)
    unsigned int   _keepAwake;   // IOPMAssertion id held while any recording runs (0 == none)
    IOPMAssertionID _keepDisplayAwake;  // display-sleep/screen-saver assertion held while fullscreen or video-only
    // Recording scheduler (Phase 5/6): a DEDICATED headless recorder drives scheduled + rule
    // recordings, independent of the visible panes' manual recorders. A ~30s tick applies the
    // shared planScheduler() core and expands EPG rules.
    std::unique_ptr<VlcPlayerMac> _scheduleRecorder;
    NSTimer*       _schedulerTimer;
    long long      _activeScheduleId;   // the schedule currently on _scheduleRecorder (0 == none)
    BOOL           _schedulerReconciled; // one-time startup reset of stale "Recording" rows
    long long      _rulesExpandedAt;    // last rule-expansion time (throttled; 0 == never)
    id             _recordingsWC;       // RecordingsWindowController (lazily created; app-lifetime)
    NSView*        _topBar;      // top command bar (add / settings / search / filter)
    NSSplitView*   _split;       // channel grid | video
    BOOL           _gridHidden;    // View ▸ Hide Channel List
    BOOL           _toolbarHidden; // View ▸ Hide Toolbar
    BOOL           _videoOnly;     // View ▸ Video Only — all chrome hidden, video fills
    BOOL           _metersHidden;  // bottom-bar toggle: hide the whole meter line
    CGFloat        _meterPosX, _meterPosY;  // meter-bar position as a 0..1 fraction of the pane
    BOOL           _pipMoved;               // the user moved/resized the PiP inset (else default corner)
    CGFloat        _pipPosX, _pipPosY;      // PiP inset position, as a 0..1 fraction of the free travel
    CGFloat        _pipW, _pipH;            // PiP inset size in px (persisted); 0 == use the default
    BOOL           _pipResizing;            // latched at drag-start: this drag resizes (else moves)
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
    uint64_t                      _epgToken;               // only the newest guide refresh's result applies
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

    std::wstring err;
    if (!_db->open(Database::defaultDbPath(), &err)) {
        diag::error(L"DB open failed: " + err);
        _db.reset();  // make the unusable state explicit; the guards below surface it
    }

    // Terms-of-Use gate (peer of the Win32 gate): the user must accept before the app is
    // usable, and must RE-ACCEPT on every version change (fresh install OR update).
    // `tos_accepted` stores the full version it was last accepted for, so any bump
    // re-prompts; every other launch is silent. Runs before the window is shown; declining
    // quits the app.
    if (_db) {
        const std::wstring tosVer = RE_VERSION_FULL_W;
        const auto accepted = _db->getSetting(L"tos_accepted");
        if (!accepted || *accepted != tosVer) {
            TermsDialog* terms = [[TermsDialog alloc] initWithVersion:ns(RE_VERSION_W)];
            const BOOL ok = [terms runModal];
            [terms release];  // MRC: balance the alloc (runModal has returned)
            if (!ok) {
                diag::info(L"Terms declined — exiting");
                [NSApp terminate:nil];
                return;
            }
            _db->setSetting(L"tos_accepted", tosVer);
            diag::info(L"Terms accepted for " + tosVer);
        }
    }

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
    _status.frame = NSMakeRect(12, 3, cs.width - 24 - 150, statusH - 5);
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
    _meterBtn.frame = NSMakeRect(cs.width - 162, 1, 24, 20);
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
    [self addColumn:@"name" title:Tr(StringId::LabelChannel) width:200];
    [self addColumn:@"group" title:Tr(StringId::GridHeaderGroup) width:120];
    _table.headerView = [[NSTableHeaderView alloc] init];
    _table.dataSource = self;
    _table.delegate = self;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.allowsColumnResizing = YES;
    _table.menu = [self makeRowMenu];  // right-click: Play / Toggle Favourite
    scroll.documentView = _table;
    [split addSubview:scroll];

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
    _meterBar.onMoved = ^{ [me persistMeterPos]; };
    static const MeterKind meterKinds[4] = {MeterKind::Spectrum, MeterKind::Signal,
                                            MeterKind::Bitrate, MeterKind::Frames};
    for (int k = 0; k < 4; ++k) {
        _meters[k] = [[MeterView alloc] initWithKind:meterKinds[k]];
        _meters[k].hidden = YES;
        [_meterBar addSubview:_meters[k]];
    }
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
    [self applyMeterConfig];        // configure + show the enabled meters (start the tap if Spectrum is on)

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into

    // Poll libVLC stream stats every 250ms to drive the meter (no audio capture).
    _bitrateMax = 0.0;
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self
                                                 selector:@selector(tickStats) userInfo:nil repeats:YES];

    [self restoreLastPlaylist];
    [self startScheduler];  // begin the ~30s recording-scheduler tick

    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
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

- (void)setStatus:(NSString*)s { _status.stringValue = s; }

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

    // Filter popup — rebuildFilterMenu re-reads its Tr labels but force-selects index 0; preserve the
    // user's current filter across the rebuild (the tag+representedObject dance from
    // -reloadAfterPlaylistChange). Group/country names ride on representedObject as DATA, untranslated.
    NSMenuItem* prev = _filter.selectedItem;
    const NSInteger prevTag = prev ? prev.tag : kFilterAll;
    // RETAIN across the rebuild: rebuildFilterMenu's [removeAllItems] releases the old items, each
    // of which owns its representedObject string — so a bare pointer here would dangle before the
    // isEqualToString: below (MRC use-after-free, adversarially reproduced). autorelease so it lives.
    NSString* const prevRep = prev ? [[(NSString*)prev.representedObject retain] autorelease] : nil;
    [self rebuildFilterMenu];
    for (NSMenuItem* it in _filter.itemArray) {
        if (it.isSeparatorItem) continue;
        NSString* rep = (NSString*)it.representedObject;
        const BOOL repEq = (!rep && !prevRep) || (rep && prevRep && [rep isEqualToString:prevRep]);
        if (it.tag == prevTag && repEq) { [_filter selectItem:it]; break; }
    }

    // Status line — best-effort re-derive (a transient in-progress message can't be reconstructed;
    // it re-renders in the new language on the next event). Playing → StatusPlaying, else the count.
    if (ap && ap->channelId)
        [self setStatus:TrF(StringId::StatusPlaying, {ns(ap->channel.name)})];
    else
        [self setStatus:TrF(StringId::MacMainWindowChannelCountStatus,
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
    }
    // Rebuild happens only on playlist load — always reset to "All channels" so a
    // switch/import shows the new playlist in full, never a stale group filter.
    [_filter selectItemAtIndex:0];
}

// Apply the current search text + filter selection and reload the table.
- (void)refreshChannels {
    if (!_db) { _channels.clear(); [_table reloadData]; return; }
    const std::wstring q = ws(_search.stringValue);
    NSMenuItem* sel = _filter.selectedItem;

    // The channel set for the active filter (favourites / group / country / all).
    auto filteredSet = [&]() -> std::vector<Channel> {
        switch (sel.tag) {
            case kFilterFavourites: return _db->favourites();
            case kFilterGroup:      return _db->channelsByGroup(ws(sel.representedObject));
            case kFilterCountry:    return _db->channelsByCountry(ws(sel.representedObject));
            default:                return _currentPid ? _db->channelsByPlaylist(_currentPid)
                                                       : _db->allChannels();
        }
    };

    if (q.empty()) {
        _channels = filteredSet();
    } else if (sel.tag == kFilterAll) {
        _channels = _db->searchChannels(q);  // search across the full (all-channels) view
    } else {
        // Search AND filter both apply: intersect the search hits with the filter
        // set by id, so e.g. ★ Favourites + "news" shows favourites matching "news".
        std::set<long long> keep;
        for (const auto& c : filteredSet()) keep.insert(c.id);
        _channels.clear();
        for (auto& c : _db->searchChannels(q))
            if (keep.count(c.id)) _channels.push_back(std::move(c));
    }

    // Categories include-filter: keep only channels whose group is checked (blank groups always show,
    // matching Win32). Applied after the popup/search filters so it composes with them.
    if (!_categoryFilter.empty()) {
        std::vector<Channel> kept;
        kept.reserve(_channels.size());
        for (auto& c : _channels)
            if (c.groupTitle.empty() || _categoryFilter.count(c.groupTitle))
                kept.push_back(std::move(c));
        _channels = std::move(kept);
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
    _emptyHint.hidden = !_channels.empty() || _viewMode != ViewMode::Single;
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

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _search) [self refreshChannels];  // live search
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
    if (p->player->isRecording()) {
        const std::wstring file = p->player->recordingFile();
        p->player->stopRecordingAsync();  // off-main: a stalled feed can't hang the UI on stop
        [self updateRecordButton];
        [self syncKeepAwake];  // another pane may still record — re-derive, don't assume
        [self setStatus:TrF(StringId::StatusRecordingSaved, {ns(file)})];
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
    if (_keepAwake != 0) { IOPMAssertionRelease(_keepAwake); _keepAwake = 0; }
    [_schedulerTimer invalidate]; _schedulerTimer = nil;
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

- (void)schedulerTick {
    if (!_db) return;
    auto schedules = _db->listSchedules();

    // One-time startup reconcile: a row still marked Recording is stale (a prior session closed
    // mid-record; nothing is actually recording now). Reset to Pending so planScheduler resumes
    // it if still in-window, or misses it. (Peer of Win32's schedulerReconciled.)
    if (!_schedulerReconciled) {
        _schedulerReconciled = YES;
        bool changed = false;
        for (const auto& s : schedules)
            if (s.status == rabbitears::ScheduleStatus::Recording) {
                _db->updateScheduleStatus(s.id, rabbitears::ScheduleStatus::Pending);
                changed = true;
            }
        if (changed) schedules = _db->listSchedules();
    }

    const long long now = (long long)time(nullptr);
    // The dedicated recorder is used ONLY for schedules, so no "manual recording" blocks it.
    const rabbitears::SchedulerPlan plan = rabbitears::planScheduler(schedules, now, /*manual=*/false);

    for (long long id : plan.stop) {
        if (id == _activeScheduleId) [self stopScheduledRecorder];
        _db->updateScheduleStatus(id, rabbitears::ScheduleStatus::Done);
        [self setStatus:Tr(StringId::StatusScheduledRecordingSaved)];
    }
    for (long long id : plan.miss)
        _db->updateScheduleStatus(id, rabbitears::ScheduleStatus::Missed);
    for (long long id : plan.start) {  // planScheduler yields at most one
        const rabbitears::ScheduledRecording* s = nullptr;
        for (const auto& x : schedules) if (x.id == id) { s = &x; break; }
        if (!s) continue;
        NSString* ext; std::string mux;
        [MainWindowController extForFormat:s->mux ext:&ext mux:&mux];
        NSString* path = [self recordingPathFor:ns(s->channelName) ext:ext];
        if (_scheduleRecorder &&
            _scheduleRecorder->startRecording(s->streamUrl, s->userAgent, s->referrer, ws(path), mux)) {
            _activeScheduleId = id;
            _db->updateScheduleStatus(id, rabbitears::ScheduleStatus::Recording, ws(path));
            [self setStatus:TrF(StringId::StatusRecordingScheduledNow, {ns(s->channelName)})];
        } else {
            _db->updateScheduleStatus(id, rabbitears::ScheduleStatus::Failed);
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

    const FlowStats fs = _player->sampleStats();
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
    }
}

// Push the loaded config into the views, relayout, and match the Spectrum tap to its
// enabled state (creating the tap is what triggers the one-time consent prompt).
- (void)applyMeterConfig {
    for (int k = 0; k < 4; ++k) [_meters[k] setConfig:_meterCfg[k]];
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

    // Lay the enabled meters left-to-right inside the bar; size the bar to fit them.
    CGFloat x = 0;
    for (int k = 0; k < 4; ++k) {
        const BOOL show = _meterCfg[k].enabled && !_videoOnly && !_metersHidden;
        _meters[k].hidden = !show;
        if (!show) continue;
        _meters[k].frame = NSMakeRect(x, 0, kMeterW[k], kMeterH);
        x += kMeterW[k] + kMeterGap;
    }
    const CGFloat barW = x > 0 ? x - kMeterGap : 0;  // drop the trailing gap
    _meterBar.hidden = (barW <= 0);
    if (barW <= 0) return;
    // Position the bar at the persisted fraction of the available range, clamped.
    const CGFloat availX = std::max(0.0, W - barW), availY = std::max(0.0, H - kMeterH);
    _meterBar.frame = NSMakeRect(_meterPosX * availX, _meterPosY * availY, barW, kMeterH);
}

- (void)videoPaneResized:(NSNotification*)__unused n { [self applyVideoPaneLayout]; [self applyMeterLayout]; }

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
        CGFloat newW = std::clamp<CGFloat>(f.size.width  - d.x, kPipMinW, maxW);
        CGFloat newH = std::clamp<CGFloat>(f.size.height + d.y, kPipMinH, maxH);
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
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        dying->stop();
        delete dying;  // ~VlcPlayerMac: media_player_stop + release (blocking, off-main)
        dispatch_async(dispatch_get_main_queue(), ^{ [view release]; });  // now safe to free
    });
}

// Switch view mode. Split uses `count` panes (2×2 = 4). Grows/shrinks the pane set,
// carrying the active stream into pane 0 when collapsing away from it.
- (void)applyViewMode:(ViewMode)mode paneCount:(int)count {
    if (mode == ViewMode::Single) count = 1;
    if (count < 1) count = 1;
    _viewMode = mode;
    // NB: the PiP inset position/size is PERSISTED (see -paneDragged: / -persistPipGeometry),
    // so entering PiP restores where the user last left it rather than snapping to the corner.

    while ((int)_panes.size() < count) [self makePane];   // grow

    if ((int)_panes.size() > count) {                     // shrink
        if (_activePane >= count) { [self carryStreamFromPane:_activePane toPane:0]; _activePane = 0; }
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

    // Channels ▸ — favourites import/export + Categories include-filter (Win32 also carries
    // Hide-unavailable, which the mac channel list does not have).
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem* chanItem = [m addItemWithTitle:Tr(StringId::MenuChannels) action:nil keyEquivalent:@""];
    NSMenu* chan = [[NSMenu alloc] init];
    [[chan addItemWithTitle:Tr(StringId::MenuImportFavourites) action:@selector(importFavourites:) keyEquivalent:@""] setTarget:self];
    [[chan addItemWithTitle:Tr(StringId::MenuExportFavourites) action:@selector(exportFavourites:) keyEquivalent:@""] setTarget:self];
    [chan addItem:[NSMenuItem separatorItem]];
    [[chan addItemWithTitle:Tr(StringId::MenuCategories) action:@selector(showCategories:) keyEquivalent:@""] setTarget:self];
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

    [[m addItemWithTitle:Tr(StringId::MenuMeters) action:@selector(showMeters:) keyEquivalent:@""] setTarget:self];

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
    if (!_recordingsWC) _recordingsWC = [[RecordingsWindowController alloc] initWithDatabase:_db.get()];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    [(RecordingsWindowController*)_recordingsWC presentRelativeTo:_window onChange:^{
        [me expandRecordingRules:YES];  // a re-enabled rule should re-queue now
        [me schedulerTick];
    }];
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

// Settings ▾ ▸ Meters… — the config dialog (peer of Win32 Settings → Meters).
- (void)showMeters:(id)__unused sender {
    MetersDialog* dlg = [[MetersDialog alloc] initWithDatabase:_db.get()];
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
    NSMenuItem* prev = _filter.selectedItem;
    const NSInteger prevTag = prev ? prev.tag : kFilterAll;
    // RETAIN across the rebuild: rebuildFilterMenu's [removeAllItems] releases the old items, each
    // of which owns its representedObject string — so a bare pointer here would dangle before the
    // isEqualToString: below (MRC use-after-free, adversarially reproduced). autorelease so it lives.
    NSString* const prevRep = prev ? [[(NSString*)prev.representedObject retain] autorelease] : nil;
    [self rebuildFilterMenu];
    if (currentOK) {
        for (NSMenuItem* it in _filter.itemArray) {
            if (it.isSeparatorItem) continue;
            NSString* rep = (NSString*)it.representedObject;
            const BOOL repEq = (!rep && !prevRep) || (rep && prevRep && [rep isEqualToString:prevRep]);
            if (it.tag == prevTag && repEq) { [_filter selectItem:it]; break; }
        }
    }
    [self refreshChannels];
    if (!_currentPid && lastEnabled == 0 && !playlists.empty())
        [self setStatus:Tr(StringId::MacMainWindowAllPlaylistsDisabledSee)];
}

// ---- TV Guide (EPG) ----------------------------------------------------------
// Peer of the Win32 onEpgRefresh/onEpgDone (MainWindowCommands.cpp): download + gunzip
// + parse the XMLTV guide for every enabled playlist that carries a guide URL, off the
// main queue, then store the parsed programmes on the main thread. Newest refresh wins.

- (void)refreshGuide:(id)__unused sender {
    if (!_db) { [self setStatus:Tr(StringId::MacMainWindowDbUnavailable)]; return; }
    struct Target { long long id; std::wstring name; std::wstring url; };
    auto targets = std::make_shared<std::vector<Target>>();
    for (const auto& pl : _db->listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets->push_back({pl.id, pl.name, pl.epgUrl});
    if (targets->empty()) {
        [self showResults:Tr(StringId::RefreshGuideNoSourceHeading)
                     info:Tr(StringId::MacMainWindowNoGuideSourceBody)];
        return;
    }
    [self setStatus:Tr(StringId::StatusDownloadingGuide)];
    const uint64_t token = ++_epgToken;  // newest refresh wins (an in-flight one is superseded)
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        // Each fetch's outcome: parsed programmes, or an error string.
        struct Fetch { long long id; std::wstring name; std::wstring error; std::vector<Programme> progs; };
        auto results = std::make_shared<std::vector<Fetch>>();
        for (const auto& t : *targets) {
            Fetch f; f.id = t.id; f.name = t.name;
            std::string bytes; std::wstring err;
            if (!httpGet(t.url, bytes, err, 60000)) {  // guides are large; allow 60s
                f.error = err.empty() ? L"download failed" : err;
            } else {
                const std::string xml = gunzipIfNeeded(bytes);  // .xml.gz bodies arrive still-compressed
                if (xml.empty()) f.error = L"empty or invalid after decompression";
                else f.progs = parseXmltv(xml).programmes;
            }
            results->push_back(std::move(f));
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != self->_epgToken || !self->_db) return;  // a newer refresh superseded this one
            const long long now = (long long)time(nullptr);  // stamp at store-time (peer of Win32 onEpgDone)
            int okCount = 0, totalProg = 0;
            std::set<std::wstring> chans;
            NSMutableString* detail = [NSMutableString string];
            for (auto& f : *results) {
                if (!f.error.empty()) {
                    [detail appendString:TrF(StringId::MacMainWindowEpgErrorLine, {ns(f.name), ns(f.error)})];
                    continue;
                }
                const int stored = self->_db->bulkInsertProgrammes(f.id, f.progs, now);
                ++okCount; totalProg += stored;
                for (const auto& p : f.progs) chans.insert(p.channelId);
                [detail appendString:TrF(StringId::EpgDetailProgrammesLine,
                    {ns(f.name), [NSString stringWithFormat:@"%d", stored]})];
            }
            NSString* summary = okCount > 0
                ? TrF(StringId::EpgStoredSummary,
                   {[NSString stringWithFormat:@"%d", totalProg],
                    [NSString stringWithFormat:@"%lu", (unsigned long)chans.size()]})
                : Tr(StringId::EpgRefreshFailedSummary);
            [self setStatus:summary];
            if (okCount > 0) [self expandRecordingRules:YES];  // new airings → re-queue series rules
            [self showResults:summary info:detail];
        });
    });
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
        std::set<std::wstring> wantUrls, wantTvgs;   // exported (from the file)
        for (const auto& pc : doc.channels) {
            wantUrls.insert(pc.streamUrl);
            if (!pc.tvgId.empty()) wantTvgs.insert(pc.tvgId);
        }
        std::set<std::wstring> libUrls, libTvgs;     // present (in the library)
        std::set<long long> toFav;
        for (const auto& c : _db->allChannels()) {
            libUrls.insert(c.streamUrl);
            if (!c.tvgId.empty()) libTvgs.insert(c.tvgId);
            // Favourite this row if its URL was exported, else if its tvg-id was (URL preferred).
            if (wantUrls.count(c.streamUrl) ||
                (!c.tvgId.empty() && wantTvgs.count(c.tvgId)))
                toFav.insert(c.id);
        }
        // An exported entry is "unmatched" when nothing in the library shares its URL or tvg-id.
        int unmatched = 0;
        for (const auto& pc : doc.channels)
            if (!libUrls.count(pc.streamUrl) &&
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
    MacVideoPane* p = _panes[(size_t)idx].get();
    p->player->play(c.streamUrl, c.userAgent, c.referrer);
    p->player->setVolume((int)_volume.doubleValue);  // libVLC resets volume per media
    p->player->setMuted(idx != _activePane);         // single-audio: only the active pane
    p->channel = c;
    p->channelId = c.id;
    if (idx != _activePane) {
        [self setStatus:TrF(StringId::MacMainWindowPlayingInPane,
                         {Tr(_viewMode == ViewMode::Pip ? StringId::MacMainWindowPipShort
                                                        : StringId::MacMainWindowBackgroundPane),
                          ns(c.name)})];
        return;
    }
    if (_meterCfg[(int)MeterKind::Spectrum].enabled) [self startSpectrumTap];  // Spectrum is opt-in
    if (_db) _db->setSetting(L"last_channel_id", std::to_wstring(c.id));  // resume this on next launch
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
    if ([id_ isEqualToString:@"group"]) return ns(c.groupTitle);
    return ns(c.name);  // "name"
}

@end
