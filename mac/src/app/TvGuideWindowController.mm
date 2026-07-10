// SPDX-License-Identifier: GPL-3.0-or-later
// See TvGuideWindowController.h.
#import "TvGuideWindowController.h"

#import "EpgGuideView.h"

#include "db/Database.h"
#include "models/Channel.h"
#include "models/Programme.h"
#include "platform/Encoding.h"

#include <ctime>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using rabbitears::Channel;
using rabbitears::Database;
using rabbitears::Programme;

namespace {
NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:rabbitears::utf8FromWide(w).c_str()] ?: @"";
}
std::wstring ws(NSString* s) { return rabbitears::wideFromUtf8(s.UTF8String ?: ""); }

// iptv-org tvg-ids carry an "@feed" quality suffix (e.g. "CNN.us@SD") while XMLTV feeds
// key on the base id ("CNN.us"); match on the lowercased base (peer of the Win32 normId).
std::wstring normId(const std::wstring& s) {
    std::wstring b = s.substr(0, s.find(L'@'));
    for (auto& ch : b)
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    return b;
}
}  // namespace

@interface TvGuideWindowController () <EpgGuideViewDelegate>
@end

@implementation TvGuideWindowController {
    Database*     _db;
    NSWindow*     _window;
    EpgGuideView* _guideView;
    void (^_onPlay)(NSString*, NSString*);
}

- (instancetype)initWithDatabase:(Database*)db {
    if ((self = [super init])) _db = db;
    return self;
}

// Assemble guide rows from stored programmes across all enabled playlists (peer of the
// Win32 onEpgGuide): group programmes by channel, keep only channels present in a playlist
// (so every row is playable), resolve the tvg-id via the lowercased base, sort by name.
- (NSArray<REGuideRow*>*)buildRows {
    NSMutableArray<REGuideRow*>* out = [NSMutableArray array];
    if (!_db) return out;
    const long long now = (long long)time(nullptr);
    const long long winStart = now - 6 * 3600;    // a little history
    const long long winEnd   = now + 72 * 3600;   // three days ahead

    for (const auto& pl : _db->listPlaylists()) {
        if (!pl.enabled) continue;
        std::vector<Programme> progs = _db->programmesInWindow(pl.id, winStart, winEnd);  // ordered channel,start
        if (progs.empty()) continue;

        // base tvg-id -> (channel name, full tvg-id). Keep the first channel per base.
        std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> byBase;
        for (const auto& c : _db->channelsByPlaylist(pl.id))
            if (!c.tvgId.empty()) byBase.emplace(normId(c.tvgId), std::make_pair(c.name, c.tvgId));

        REGuideRow* cur = nil;
        NSMutableArray<REGuideProgramme*>* curProgs = nil;
        std::wstring curId;
        bool started = false;
        for (auto& p : progs) {
            if (!started || p.channelId != curId) {
                if (cur && curProgs.count) { cur.programmes = curProgs; [out addObject:cur]; }
                cur = nil; curProgs = nil;
                curId = p.channelId;
                started = true;
                auto it = byBase.find(normId(curId));  // programme.channelId is the EPG base id
                if (it != byBase.end()) {
                    cur = [[REGuideRow alloc] init];
                    cur.channelId = ns(it->second.second);                                   // full tvg-id
                    cur.channelName = ns(it->second.first.empty() ? curId : it->second.first);
                    curProgs = [NSMutableArray array];
                }
            }
            if (cur) {
                REGuideProgramme* gp = [[REGuideProgramme alloc] init];
                gp.title = p.title.empty() ? @"(no title)" : ns(p.title);
                gp.descr = ns(p.descr);
                gp.startUtc = p.startUtc;
                gp.stopUtc = p.stopUtc;
                [curProgs addObject:gp];
            }
        }
        if (cur && curProgs.count) { cur.programmes = curProgs; [out addObject:cur]; }
    }
    [out sortUsingComparator:^NSComparisonResult(REGuideRow* a, REGuideRow* b) {
        return [a.channelName localizedCaseInsensitiveCompare:b.channelName];
    }];
    return out;
}

- (void)presentRelativeTo:(NSWindow*)parent
                   onPlay:(void (^)(NSString*, NSString*))onPlay {
    [self presentRelativeTo:parent showChannel:nil onPlay:onPlay];
}

- (REGuideShowResult)presentRelativeTo:(NSWindow*)parent
                           showChannel:(NSString*)tvgId
                                onPlay:(void (^)(NSString*, NSString*))onPlay {
    _onPlay = [onPlay copy];
    NSArray<REGuideRow*>* rows = [self buildRows];
    if (rows.count == 0) {
        NSAlert* a = [[NSAlert alloc] init];
        a.messageText = @"No guide to show";
        a.informativeText =
            @"No stored programmes match a channel in your playlists.\n\n"
            @"Run Refresh Guide first — or the guide's channel IDs don't line up with your "
            @"playlist. Point it at a guide whose tvg-ids match (Manage Playlists ▸ 📅 Set Guide URL).";
        [a addButtonWithTitle:@"OK"];
        [a beginSheetModalForWindow:(parent ?: NSApp.keyWindow)
                  completionHandler:^(NSModalResponse __unused r) {}];
        return REGuideShowNoGuide;
    }
    if (!_window) [self buildWindowNear:parent];
    [_guideView setRows:rows nowUtc:(long long)time(nullptr)];
    [_window makeKeyAndOrderFront:nil];

    if (tvgId.length == 0) return REGuideShowRevealed;
    // Find the row whose channel resolves to the same base tvg-id (rows are sorted by name,
    // so scan _guideView.rows — the exact array the view is addressing by index).
    const std::wstring want = normId(ws(tvgId));
    NSInteger found = -1;
    NSArray<REGuideRow*>* shown = _guideView.rows;
    for (NSInteger i = 0; i < (NSInteger)shown.count; ++i)
        if (normId(ws(shown[(NSUInteger)i].channelId)) == want) { found = i; break; }
    if (found < 0) return REGuideShowChannelMissing;
    [_guideView revealRowAtIndex:found];
    return REGuideShowRevealed;
}

- (void)buildWindowNear:(NSWindow*)parent {
    const NSRect frame = NSMakeRect(0, 0, 940, 580);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"TV Guide";
    _window.releasedWhenClosed = NO;  // we own it via the ivar; a later re-open reuses it
    _window.contentMinSize = NSMakeSize(560, 360);

    _guideView = [[EpgGuideView alloc] initWithFrame:frame];
    _guideView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _guideView.delegate = self;
    _window.contentView = _guideView;

    _window.frameAutosaveName = @"RabbitEarsGuideWindow";  // remember size/position
    if (![_window setFrameUsingName:@"RabbitEarsGuideWindow"]) {
        if (parent) {  // offset from the main window on first open
            const NSRect pf = parent.frame;
            [_window setFrameOrigin:NSMakePoint(NSMinX(pf) + 30,
                                                NSMaxY(pf) - frame.size.height - 30)];
        } else {
            [_window center];
        }
    }
}

- (void)hide { [_window orderOut:nil]; }

// A programme block was clicked: show its details with a Play action.
- (void)guideView:(EpgGuideView*)__unused view
    didActivateProgramme:(REGuideProgramme*)programme
                   inRow:(REGuideRow*)row {
    NSDateFormatter* fmt = [[NSDateFormatter alloc] init];
    fmt.dateStyle = NSDateFormatterMediumStyle;
    fmt.timeStyle = NSDateFormatterShortStyle;
    NSString* start = programme.startUtc
        ? [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)programme.startUtc]]
        : @"";
    NSDateFormatter* tf = [[NSDateFormatter alloc] init];
    tf.timeStyle = NSDateFormatterShortStyle;
    NSString* stop = programme.stopUtc
        ? [tf stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)programme.stopUtc]]
        : @"";

    NSMutableString* info = [NSMutableString stringWithFormat:@"%@\n%@ – %@",
                             row.channelName, start, stop];
    if (programme.descr.length) [info appendFormat:@"\n\n%@", programme.descr];

    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = programme.title.length ? programme.title : @"Programme";
    a.informativeText = info;
    NSButton* play = [a addButtonWithTitle:@"Play Channel"];
    play.enabled = row.channelId.length > 0;
    NSButton* close = [a addButtonWithTitle:@"Close"];
    close.keyEquivalent = @"\033";  // Esc closes

    __weak TvGuideWindowController* weak = self;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        TvGuideWindowController* s = weak;
        if (!s) return;
        if (resp == NSAlertFirstButtonReturn && s->_onPlay && row.channelId.length)
            s->_onPlay(row.channelId, row.channelName);
    }];
}

@end
