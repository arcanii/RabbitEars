// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <ctime>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
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
#import "SpectrumTap.h"
#import "TermsDialog.h"
#import "TvGuideWindowController.h"
#import "VlcEngineMac.h"
#import "VlcPlayerMac.h"

using namespace rabbitears;
using namespace rabbitears::mac;  // MeterKind / MeterStyle / MeterConfig + the meter codecs

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

// Filter popup tags.
enum { kFilterAll = 0, kFilterFavourites = 1, kFilterGroup = 2, kFilterCountry = 3 };

// Chrome metrics, shared by showWindow and the hide/show-chrome relayout.
static const CGFloat kBarH = 46;     // top command bar height
static const CGFloat kStatusH = 22;  // bottom status/volume bar height
static const CGFloat kMeterH = 24;   // meter line height
static const CGFloat kMeterGap = 8;  // gap between adjacent meters
// Per-kind meter widths (index = (int)MeterKind: Spectrum, Signal, Bitrate, Frames).
static const CGFloat kMeterW[4] = {180, 64, 130, 96};

@implementation MainWindowController {
    NSWindow*      _window;
    NSSearchField* _search;
    NSPopUpButton* _filter;
    RETableView*   _table;
    NSView*        _videoView;
    MeterView*     _meters[4];   // Spectrum / Signal / Bitrate / Frames (index = (int)MeterKind)
    DraggableMeterBar* _meterBar;  // floats the meters over the video; user-draggable
    NSTextField*   _emptyHint;   // "no channels yet" hint centered over the empty video pane
    NSTextField*   _status;
    NSSlider*      _volume;      // bottom-bar volume (0..100)
    NSButton*      _muteBtn;     // 🔊 / 🔇 toggle
    NSButton*      _meterBtn;    // bottom-bar show/hide-meters toggle
    NSView*        _topBar;      // top command bar (add / settings / search / filter)
    NSSplitView*   _split;       // channel grid | video
    BOOL           _gridHidden;    // View ▸ Hide Channel List
    BOOL           _toolbarHidden; // View ▸ Hide Toolbar
    BOOL           _videoOnly;     // View ▸ Video Only — all chrome hidden, video fills
    BOOL           _metersHidden;  // bottom-bar toggle: hide the whole meter line
    CGFloat        _meterPosX, _meterPosY;  // meter-bar position as a 0..1 fraction of the pane
    id             _escMonitor;    // local key monitor for Esc while video-only (nil otherwise)

    std::unique_ptr<Database>     _db;
    std::unique_ptr<VlcEngineMac> _engine;   // shared libVLC instance; every player borrows it
    std::unique_ptr<VlcPlayerMac> _player;
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
        _player = std::make_unique<VlcPlayerMac>();
        _player->init(*_engine);
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
    _window.title = @"RabbitEars";
    [_window center];
    _window.releasedWhenClosed = NO;  // we own it via the ivar
    _window.contentMinSize = NSMakeSize(560, 360);  // keep both split panes usable
    _window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;  // ⌃⌘F / green button
    _window.frameAutosaveName = @"RabbitEarsMainWindow";  // remember size + position across launches
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
    NSButton* addBtn = [NSButton buttonWithTitle:@"+  Add Playlist"
                                          target:self action:@selector(addPlaylist:)];
    addBtn.frame = NSMakeRect(12, 9, 138, 28);
    addBtn.bezelColor = NSColor.controlAccentColor;  // accent, like the Win32 button
    [bar addSubview:addBtn];

    // A gear that pops the Open/Manage/Meters/Updates/About menu (showSettings:).
    NSImage* gear = [NSImage imageWithSystemSymbolName:@"gearshape" accessibilityDescription:@"Settings"];
    NSButton* setBtn = [NSButton buttonWithImage:gear target:self action:@selector(showSettings:)];
    setBtn.frame = NSMakeRect(158, 9, 40, 28);
    setBtn.toolTip = @"Settings";
    [bar addSubview:setBtn];

    // Right (pinned): Stop, the filter popup, and the stretchy search field between.
    NSButton* stopBtn = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stop:)];
    stopBtn.frame = NSMakeRect(cs.width - 92, 9, 80, 28);
    stopBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:stopBtn];

    _filter = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(cs.width - 270, 9, 170, 28) pullsDown:NO];
    _filter.target = self;
    _filter.action = @selector(filterChanged:);
    _filter.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:_filter];

    _search = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(284, 10, cs.width - 562, 26)];
    _search.placeholderString = @"Search channels…";
    _search.autoresizingMask = NSViewWidthSizable;
    _search.delegate = self;  // controlTextDidChange: -> live filter
    [bar addSubview:_search];

    [content addSubview:bar];

    // ---- bottom bar: status line (left) + volume (right) ----
    const CGFloat statusH = kStatusH;
    _status = [NSTextField labelWithString:@"Ready."];
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
                                                  accessibilityDescription:@"Show/Hide meters"]
                                   target:self action:@selector(toggleMeters:)];
    _meterBtn.bordered = NO;
    _meterBtn.toolTip = @"Show / hide the meters";
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
    [self addColumn:@"name" title:@"Channel" width:200];
    [self addColumn:@"group" title:@"Group" width:120];
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
    _videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, kMeterH, 600, 100 - kMeterH)];
    _videoView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _videoView.wantsLayer = YES;
    _videoView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [videoPane addSubview:_videoView];
    NSClickGestureRecognizer* dbl =
        [[NSClickGestureRecognizer alloc] initWithTarget:self action:@selector(videoDoubleClicked:)];
    dbl.numberOfClicksRequired = 2;
    [_videoView addGestureRecognizer:dbl];
    [dbl release];  // MRC: the view retains it

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
    _emptyHint = [NSTextField labelWithString:@"No channels yet — click “＋ Add Playlist” to begin."];
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
    [self applyMeterConfig];        // configure + show the enabled meters (start the tap if Spectrum is on)

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into

    // Poll libVLC stream stats every 250ms to drive the meter (no audio capture).
    _bitrateMax = 0.0;
    _statsTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self
                                                 selector:@selector(tickStats) userInfo:nil repeats:YES];

    [self restoreLastPlaylist];

    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
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
    [[m addItemWithTitle:@"Play" action:@selector(playClicked:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Toggle Favourite" action:@selector(toggleFavourite:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Set Channel Number…" action:@selector(editChannelNumber:) keyEquivalent:@""] setTarget:self];
    return m;
}

- (void)setStatus:(NSString*)s { _status.stringValue = s; }

// ---- playlist load / filter model ----

- (void)restoreLastPlaylist {
    if (!_db) { [self setStatus:@"Database unavailable — check disk permissions and restart."]; return; }
    const auto playlists = _db->listPlaylists();
    long long pid = 0;
    for (const auto& p : playlists) if (p.enabled) pid = p.id;  // last enabled (added_at order)
    if (!pid) {
        [self setStatus:playlists.empty()
                            ? @"Ready — load an M3U URL or open a file to begin."
                            : @"All playlists are disabled — enable one in Settings ▸ Manage Playlists…."];
        [self rebuildFilterMenu];
        return;
    }
    [self showPlaylist:pid];
    [self selectLastPlayed];
}

// Highlight (don't auto-play) the channel that was playing when the app last quit.
- (void)selectLastPlayed {
    if (!_db) return;
    const auto s = _db->getSetting(L"last_channel_id");
    if (!s || s->empty()) return;
    const long long cid = std::wcstoll(s->c_str(), nullptr, 10);
    for (size_t i = 0; i < _channels.size(); ++i) {
        if (_channels[i].id != cid) continue;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        [_table scrollRowToVisible:(NSInteger)i];
        [self setStatus:[NSString stringWithFormat:
                         @"Last played: %@ — double-click or press Return to resume.",
                         ns(_channels[i].name)]];
        return;
    }
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
    [_filter.menu addItem:it];
}

- (void)rebuildFilterMenu {
    [_filter removeAllItems];
    [self addFilterItem:@"All channels" tag:kFilterAll group:nil];
    [self addFilterItem:@"★ Favourites" tag:kFilterFavourites group:nil];
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
            [self addFilterItem:[@"Country: " stringByAppendingString:ns(cc).uppercaseString]
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

    [_table deselectAll:nil];
    [_table reloadData];
    [self setStatus:[NSString stringWithFormat:@"%lu channels%@.", (unsigned long)_channels.size(),
                     q.empty() ? @"" : @" (search)"]];
    _emptyHint.hidden = !_channels.empty();
}

// ---- actions ----

// "+ Add Playlist": prompt for a URL, then load it (peer of the Win32 onAddUrl).
- (void)addPlaylist:(id)__unused sender {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Add Playlist";
    a.informativeText = @"Playlist URL (.m3u / .m3u8):";
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
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
    [self setStatus:@"Downloading playlist…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        std::string bytes;
        std::wstring derr;
        const bool ok = httpGet(url, bytes, derr);
        auto doc = std::make_shared<M3uDocument>(ok ? parseM3u(bytes) : M3uDocument{});
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != self->_loadToken) return;  // a newer load superseded this one
            if (!ok) { [self setStatus:[NSString stringWithFormat:@"Download failed: %@", ns(derr)]]; return; }
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
        if (!perr.empty()) { [self setStatus:[NSString stringWithFormat:@"Read failed: %@", ns(perr)]]; return; }
        [self importDoc:doc name:friendlyName(path, false) source:path isUrl:false];
    }];
}

- (void)importDoc:(const M3uDocument&)doc
             name:(const std::wstring&)name
           source:(const std::wstring&)source
            isUrl:(bool)isUrl {
    if (!_db) { [self setStatus:@"Database unavailable — cannot save playlist."]; return; }
    const long long now = (long long)time(nullptr);
    // Pass doc.epgUrl (the M3U x-tvg-url) through so the playlist carries its XMLTV guide
    // source — without it, Refresh Guide has nothing to fetch (the Win32 peer at
    // MainWindowCommands.cpp:108 does the same).
    const long long pid = _db->addPlaylist(name, source, isUrl, now, doc.epgUrl);
    if (pid == 0) {
        [self setStatus:@"Add playlist failed: could not save to the database."];
        [self showResults:@"Could not import the playlist"
                     info:[NSString stringWithFormat:
                           @"Source: %@\n\nThe playlist could not be saved to the database.", ns(source)]];
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
        [self showResults:@"No channels found"
                     info:[NSString stringWithFormat:
                           @"Source: %@\n\nThe playlist contained no channels.", ns(source)]];
        return;
    }
    NSMutableString* info = [NSMutableString stringWithFormat:
        @"Source: %@\n\nChannels parsed: %d\nChannels imported: %d\n", ns(source), parsed, imported];
    if (parsed - imported > 0)
        [info appendFormat:@"Skipped (blank or duplicate URLs): %d\n", parsed - imported];
    [info appendFormat:@"Groups: %lu", (unsigned long)groups.size()];
    [self showResults:[NSString stringWithFormat:@"Imported %d channels", imported] info:info];
}

- (void)showResults:(NSString*)title info:(NSString*)info {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = info;
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse __unused r) {}];
    [a autorelease];  // MRC: the sheet retains it for its lifetime; balance the alloc +1
}

- (void)filterChanged:(id)__unused sender { [self refreshChannels]; }

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _search) [self refreshChannels];  // live search
}

- (void)stop:(id)__unused sender {
    _player->stop();
    for (MeterView* m : _meters) [m reset];
    [self stopSpectrumTap];  // free the tap's RT thread (no spinning on silence)
    _window.title = @"RabbitEars";
    [self setStatus:@"Stopped."];
}

// 250ms libVLC-stats poll → the stream-health meter. No audio capture (no consent,
// no A/V desync): the bar tracks throughput against a slowly-decaying rolling peak,
// so it fills while data flows at its usual rate and drops when the stream stalls.
- (void)tickStats {
    const FlowStats fs = _player->sampleStats();
    if (!fs.playing) { for (MeterView* m : _meters) [m reset]; _spectrumSilentTicks = 0; return; }
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
    NSView* pane = _videoView.superview;
    if (!pane) return;
    const CGFloat W = pane.bounds.size.width, H = pane.bounds.size.height;
    _videoView.frame = pane.bounds;  // video fills; the meter bar floats on top

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

- (void)videoPaneResized:(NSNotification*)__unused n { [self applyMeterLayout]; }

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
    if (!audible) { _spectrumSilentTicks = 0; return; }
    if (++_spectrumSilentTicks >= 32) [_meters[s] setAvailable:NO];  // ~8s audible but no tap energy => denied
}

// Settings pull-down (peer of the Win32 "Settings ▾" command-bar menu). Small for
// now — file import + updates/about; real preferences will hang off here.
- (void)showSettings:(NSButton*)sender {
    NSMenu* m = [[NSMenu alloc] init];
    [[m addItemWithTitle:@"Open Playlist File…" action:@selector(openFile:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Manage Playlists…" action:@selector(showPlaylists:) keyEquivalent:@""] setTarget:self];
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:@"TV Guide" action:@selector(showGuide:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Refresh Guide…" action:@selector(refreshGuide:) keyEquivalent:@""] setTarget:self];
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:@"Meters…" action:@selector(showMeters:) keyEquivalent:@""] setTarget:self];
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:@"Check for Updates…" action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"About RabbitEars" action:@selector(showAbout:) keyEquivalent:@""] setTarget:self];
    [m popUpMenuPositioningItem:nil
                     atLocation:NSMakePoint(0, NSHeight(sender.bounds) + 4)
                         inView:sender];
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

// Settings ▾ ▸ Meters… — the config dialog (peer of Win32 Settings → Meters).
- (void)showMeters:(id)__unused sender {
    MetersDialog* dlg = [[MetersDialog alloc] initWithDatabase:_db.get()];
    [dlg presentForWindow:_window onApply:^{
        [self loadMeterConfig];
        [self applyMeterConfig];
        [self setStatus:@"Meter settings saved."];
    }];
    [dlg release];  // MRC: the sheet's completion handler keeps it alive until it closes
}

// Settings ▾ ▸ Manage Playlists… — enable/disable/delete imported playlists.
- (void)showPlaylists:(id)__unused sender {
    PlaylistsDialog* dlg = [[PlaylistsDialog alloc] initWithDatabase:_db.get()];
    [dlg presentForWindow:_window onChange:^{ [self reloadAfterPlaylistChange]; }];
    [dlg release];  // MRC: the sheet's completion handler keeps it alive until it closes
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
    NSString* const prevRep = prev ? (NSString*)prev.representedObject : nil;
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
        [self setStatus:@"All playlists are disabled — enable one to see channels."];
}

// ---- TV Guide (EPG) ----------------------------------------------------------
// Peer of the Win32 onEpgRefresh/onEpgDone (MainWindowCommands.cpp): download + gunzip
// + parse the XMLTV guide for every enabled playlist that carries a guide URL, off the
// main queue, then store the parsed programmes on the main thread. Newest refresh wins.

- (void)refreshGuide:(id)__unused sender {
    if (!_db) { [self setStatus:@"Database unavailable."]; return; }
    struct Target { long long id; std::wstring name; std::wstring url; };
    auto targets = std::make_shared<std::vector<Target>>();
    for (const auto& pl : _db->listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets->push_back({pl.id, pl.name, pl.epgUrl});
    if (targets->empty()) {
        [self showResults:@"No guide source found"
                     info:@"None of your enabled playlists carry an XMLTV guide URL (the x-tvg-url in "
                          @"the #EXTM3U header).\n\nAdd a playlist that includes one, or set one per "
                          @"playlist in Manage Playlists ▸ 📅 Set Guide URL, then try again."];
        return;
    }
    [self setStatus:@"Downloading guide…"];
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
                    [detail appendFormat:@"%@:  %@\n", ns(f.name), ns(f.error)];
                    continue;
                }
                const int stored = self->_db->bulkInsertProgrammes(f.id, f.progs, now);
                ++okCount; totalProg += stored;
                for (const auto& p : f.progs) chans.insert(p.channelId);
                [detail appendFormat:@"%@:  %d programmes\n", ns(f.name), stored];
            }
            NSString* summary = okCount > 0
                ? [NSString stringWithFormat:@"Stored %d programmes across %lu channels",
                   totalProg, (unsigned long)chans.size()]
                : @"Could not refresh the guide";
            [self setStatus:summary];
            [self showResults:summary info:detail];
        });
    });
}

// Open (or re-reveal) the channels×time guide window. The window controller queries the
// DB and lays the grid out; a picked programme calls back to -playFromGuide:name:.
- (void)showGuide:(id)__unused sender {
    if (!_db) { [self setStatus:@"Database unavailable."]; return; }
    if (!_guideWC) _guideWC = [[TvGuideWindowController alloc] initWithDatabase:_db.get()];
    MainWindowController* __unsafe_unretained me = self;  // app-lifetime; no retain cycle (MRC)
    [(TvGuideWindowController*)_guideWC presentRelativeTo:_window
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
        [self showResults:@"No matching channel"
                     info:[NSString stringWithFormat:
                           @"Couldn't find a channel for “%@” in your playlists.\n\nThe guide matches "
                           @"programmes to channels by tvg-id; this programme's ID has no match. Point "
                           @"the playlist at a guide whose IDs line up (Manage Playlists ▸ 📅 Set Guide "
                           @"URL).", name]];
    }
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
    a.messageText = [NSString stringWithFormat:@"Channel number for “%@”", ns(c.name)];
    a.informativeText = @"Enter a number, or leave blank to clear it.";
    [a addButtonWithTitle:@"Set"];
    [a addButtonWithTitle:@"Cancel"];
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

// The shared "start playing this channel" path — used by the grid (playRow:) and the
// TV Guide (playFromGuide:name:).
- (void)playChannel:(const Channel&)c {
    _player->play(c.streamUrl, c.userAgent, c.referrer);
    _player->setVolume((int)_volume.doubleValue);  // libVLC resets volume per media
    if (_meterCfg[(int)MeterKind::Spectrum].enabled) [self startSpectrumTap];  // Spectrum is opt-in
    if (_db) _db->setSetting(L"last_channel_id", std::to_wstring(c.id));  // resume this on next launch
    _window.title = [NSString stringWithFormat:@"RabbitEars — %@", ns(c.name)];
    [self setStatus:[NSString stringWithFormat:@"Playing: %@", ns(c.name)]];
}

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
