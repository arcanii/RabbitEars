// SPDX-License-Identifier: GPL-3.0-or-later
//
// See AboutWindowController.h — in particular the two licence obligations this window inherited
// from the system About panel it replaced.

#import "AboutWindowController.h"

#include "Tr.h"   // brings in common/core/Strings.h (the StringId catalog)

#if __has_include("generated/version.h")
#include "generated/version.h"
#endif
#ifndef RE_VERSION_DISPLAY
#define RE_VERSION_DISPLAY "dev"  // fallback when the generated header is absent
#endif

using rabbitears::Tr;   // the AppKit NSString wrapper over the shared catalog
using rabbitears::i18n::StringId;

namespace {

// The author's tip pages. TWO backends on purpose, exactly as Win32 has it: Buy Me a Coffee takes
// card payments, Ko-fi can take others. Which methods each accepts depends on the creator's own
// settings, which is why neither button claims anything about the difference.
NSString* const kCoffeeUrl = @"https://buymeacoffee.com/bryanmarkh";
NSString* const kKofiUrl   = @"https://ko-fi.com/arcanii";

constexpr CGFloat kWinW   = 420;   // fits the disclaimer without a scroll view at every language
constexpr CGFloat kMargin = 20;

NSTextField* label(NSString* s, CGFloat size, NSFontWeight weight, NSColor* color, CGFloat width) {
    NSTextField* t = [NSTextField wrappingLabelWithString:s ?: @""];
    t.font = [NSFont systemFontOfSize:size weight:weight];
    t.textColor = color;
    t.alignment = NSTextAlignmentCenter;
    t.selectable = NO;
    t.preferredMaxLayoutWidth = width;
    [t setFrameSize:NSMakeSize(width, t.intrinsicContentSize.height)];
    return t;
}

}  // namespace

@interface AboutWindowController () <NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@end

@implementation AboutWindowController

// One instance for the app's lifetime. A local would be deallocated the moment -showAbout returned,
// taking the window's button targets with it (the classic "dialog vanishes / buttons do nothing"
// bug) — the same lifetime trap TermsDialog solves with its strong self-capture.
static AboutWindowController* g_about = nil;

+ (void)showAbout {
    if (!g_about) g_about = [[AboutWindowController alloc] init];
    [g_about present];
}

- (void)present {
    // Rebuild every time: the labels are localized, so a window built once would keep its
    // launch-language text after Settings ▸ Language switches live.
    if (self.window) {
        [self.window close];
        self.window = nil;
    }
    [self build];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)build {
    NSMutableArray<NSView*>* rows = [NSMutableArray array];
    const CGFloat textW = kWinW - kMargin * 2;

    // ---- identity ------------------------------------------------------------------------
    NSImageView* icon = [NSImageView imageViewWithImage:[NSImage imageNamed:NSImageNameApplicationIcon]];
    [icon setFrameSize:NSMakeSize(72, 72)];
    [rows addObject:icon];

    NSString* appName = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleName"] ?: @"RabbitEars";
    [rows addObject:label(appName, 15, NSFontWeightSemibold, NSColor.labelColor, textW)];

    NSString* version = @(RE_VERSION_DISPLAY);   // "0.2.17 (421)" — marketing version + build
    [rows addObject:label(version, 11, NSFontWeightRegular, NSColor.secondaryLabelColor, textW)];

    // ---- libVLC attribution + the educational-purposes disclaimer -------------------------
    // LICENCE OBLIGATION. This is the credits string the system About panel used to render.
    [rows addObject:label(Tr(StringId::AboutMacCredits), 11, NSFontWeightRegular,
                          NSColor.secondaryLabelColor, textW)];

    // ---- the GPL-3.0 notice ---------------------------------------------------------------
    // LICENCE OBLIGATION. Read from Info.plist (NSHumanReadableCopyright) rather than duplicated,
    // so the plist remains the single source of truth for it.
    NSString* copyright = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSHumanReadableCopyright"];
    if (copyright.length)
        [rows addObject:label(copyright, 10, NSFontWeightRegular, NSColor.tertiaryLabelColor, textW)];

    // ---- tip section -----------------------------------------------------------------------
    // The heading and body are not decoration: a bare "Buy me a coffee" next to the app's own
    // buttons reads like a purchase rather than a donation. Win32 carries the same two strings for
    // the same reason, and both deliberately say nothing about how the two backends differ.
    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(0, 0, textW, 1)];
    sep.boxType = NSBoxSeparator;
    [rows addObject:sep];

    [rows addObject:label(Tr(StringId::AboutTipHeading), 12, NSFontWeightSemibold,
                          NSColor.labelColor, textW)];
    [rows addObject:label(Tr(StringId::AboutTipBody), 11, NSFontWeightRegular,
                          NSColor.secondaryLabelColor, textW)];

    NSButton* coffee = [NSButton buttonWithTitle:Tr(StringId::BuyMeACoffeeButton)
                                          target:self action:@selector(openCoffee:)];
    NSButton* kofi = [NSButton buttonWithTitle:Tr(StringId::KoFiButton)
                                        target:self action:@selector(openKofi:)];
    NSButton* close = [NSButton buttonWithTitle:Tr(StringId::ButtonClose)
                                         target:self action:@selector(closeWindow:)];
    close.keyEquivalent = @"\r";                    // Return closes, as the system panel does
    for (NSButton* b in @[ coffee, kofi, close ]) [b sizeToFit];

    // Tip buttons on one row, centred; the close button on its own row below.
    const CGFloat gap = 10;
    NSView* tipRow = [[NSView alloc] initWithFrame:
        NSMakeRect(0, 0, textW, MAX(coffee.frame.size.height, kofi.frame.size.height))];
    {
        const CGFloat total = coffee.frame.size.width + gap + kofi.frame.size.width;
        CGFloat x = (textW - total) / 2;
        NSRect f = coffee.frame; f.origin = NSMakePoint(x, 0); coffee.frame = f; x += f.size.width + gap;
        f = kofi.frame;          f.origin = NSMakePoint(x, 0); kofi.frame = f;
        [tipRow addSubview:coffee];
        [tipRow addSubview:kofi];
    }
    [rows addObject:tipRow];

    NSView* closeRow = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, textW, close.frame.size.height)];
    {
        NSRect f = close.frame;
        f.origin = NSMakePoint((textW - f.size.width) / 2, 0);
        close.frame = f;
        [closeRow addSubview:close];
    }
    [rows addObject:closeRow];

    // ---- stack them bottom-up so the window height follows the CONTENT ----------------------
    // Height is measured, never assumed: the disclaimer and the tip body both wrap, and how far
    // they wrap depends on the language. A fixed height would clip Japanese or German.
    CGFloat total = kMargin;
    for (NSView* v in rows) total += v.frame.size.height + gap;
    total += kMargin - gap;

    NSRect frame = NSMakeRect(0, 0, kWinW, total);
    NSWindow* w = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = Tr(StringId::AboutWindowTitle);
    w.releasedWhenClosed = NO;   // ARC owns it through the strong property; AppKit must not free it
    w.delegate = self;

    CGFloat y = total - kMargin;
    for (NSView* v in rows) {
        NSRect f = v.frame;
        y -= f.size.height;
        f.origin = NSMakePoint(v == rows.firstObject ? (kWinW - f.size.width) / 2 : kMargin, y);
        v.frame = f;
        [w.contentView addSubview:v];
        y -= gap;
    }
    self.window = w;
}

- (void)openUrl:(NSString*)url {
    NSURL* u = [NSURL URLWithString:url];
    if (u) [NSWorkspace.sharedWorkspace openURL:u];
}

- (void)openCoffee:(id)__unused sender { [self openUrl:kCoffeeUrl]; }
- (void)openKofi:(id)__unused sender   { [self openUrl:kKofiUrl]; }
- (void)closeWindow:(id)__unused sender { [self.window close]; }

@end
