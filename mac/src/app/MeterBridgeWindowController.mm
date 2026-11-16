// SPDX-License-Identifier: GPL-3.0-or-later
// See MeterBridgeWindowController.h.
#import "MeterBridgeWindowController.h"

#import "MeterView.h"
#import "TankView.h"
#import "Tr.h"

#include <algorithm>

#include "MeterLayout.h"

using namespace rabbitears;
using namespace rabbitears::mac;
using namespace rabbitears::i18n;

@interface MeterBridgeWindowController () <NSWindowDelegate>
@end

@implementation MeterBridgeWindowController {
    NSWindow*    _window;
    MeterView*   _meters[4];
    NSTextField* _labels[4];
    MeterConfig  _cfg[4];
    BOOL         _labelsOn;
    TankView*    _tank;
    NSTextField* _tankLabel;
    BOOL         _tankOn;
}

- (instancetype)init {
    if ((self = [super init])) {
        static const MeterKind kinds[4] = {MeterKind::Spectrum, MeterKind::Signal, MeterKind::Bitrate, MeterKind::Frames};
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 150)
                                              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        _window.releasedWhenClosed = NO;
        _window.title = Tr(StringId::MeterBridgeTitle);
        _window.delegate = self;
        _window.contentMinSize = NSMakeSize(240, 60);
        _window.backgroundColor = [NSColor colorWithWhite:0.06 alpha:1.0];
        NSView* content = _window.contentView;
        for (int k = 0; k < 4; ++k) {
            _meters[k] = [[MeterView alloc] initWithKind:kinds[k]];
            _meters[k].hidden = YES;
            [content addSubview:_meters[k]];
            _labels[k] = [NSTextField labelWithString:@""];
            _labels[k].alignment = NSTextAlignmentCenter;
            _labels[k].hidden = YES;
            [content addSubview:_labels[k]];
        }
        _tank = [[TankView alloc] initWithFrame:NSMakeRect(0, 0, 92, 24)];
        _tank.hidden = YES;
        _tank.menu = nil;  // the tray's tank owns "Hide"
        [content addSubview:_tank];
        _tankLabel = [NSTextField labelWithString:@""];
        _tankLabel.hidden = YES;
        [content addSubview:_tankLabel];
        _window.frameAutosaveName = @"RabbitEarsMeterBridge";
        if (![_window setFrameUsingName:@"RabbitEarsMeterBridge"]) [_window center];
    }
    return self;
}

- (MeterView*)meterForKind:(int)k { return (k >= 0 && k < 4) ? _meters[k] : nil; }
- (TankView*)tank { return _tank; }

- (void)applyConfigs:(const MeterConfig*)cfgs labels:(BOOL)labels tank:(BOOL)tank {
    _labelsOn = labels;
    _tankOn = tank;
    _tankLabel.attributedStringValue = meterLabelText(4);
    for (int k = 0; k < 4; ++k) {
        _cfg[k] = cfgs[k];
        [_meters[k] setConfig:cfgs[k]];
        _labels[k].attributedStringValue = meterLabelText(k);
    }
    [self layout];
}

// One row, as large as the window: the tallest height at which the row fits the window's width.
- (void)layout {
    const NSSize sz = _window.contentView.bounds.size;
    const CGFloat pad = 10, labelH = _labelsOn ? kMeterLabelRowPt : 0;
    CGFloat h = std::max<CGFloat>(16, sz.height - 2 * pad - labelH);
    auto rowWidth = [&](CGFloat mh) {
        CGFloat w = 0;
        for (int k = 0; k < 4; ++k)
            if (_cfg[k].enabled) w += meterWidthPt(k, _cfg[k].style, mh, /*ownRow=*/true);
        if (_tankOn) w += kMeterGapPt + tankWidthPt(mh, true);  // the tank keeps its distance
        return w;
    };
    const CGFloat avail = sz.width - 2 * pad;
    for (int i = 0; i < 40 && rowWidth(h) > avail && h > 16; ++i) h *= 0.95;  // shrink to fit the width
    const CGFloat rowW = rowWidth(h);
    CGFloat x = std::max(pad, (sz.width - rowW) / 2);
    const CGFloat y = std::max(pad, (sz.height - h - labelH) / 2) + labelH;
    for (int k = 0; k < 4; ++k) {
        const BOOL show = _cfg[k].enabled;
        _meters[k].hidden = !show;
        _labels[k].hidden = !show || !_labelsOn;
        if (!show) continue;
        const CGFloat w = meterWidthPt(k, _cfg[k].style, h, true);
        _meters[k].frame = NSMakeRect(x, y, w, h);
        _labels[k].frame = NSMakeRect(x, y - labelH, w, labelH);
        x += w;  // side by side, like a rack of instruments
    }
    _tank.hidden = !_tankOn;
    _tankLabel.hidden = !_tankOn || !_labelsOn;
    if (_tankOn) {
        x += kMeterGapPt;
        const CGFloat w = tankWidthPt(h, true);
        _tank.frame = NSMakeRect(x, y, w, h);
        _tankLabel.frame = NSMakeRect(x, y - labelH, w, labelH);
    }
}

- (void)windowDidResize:(NSNotification*)__unused n { [self layout]; }

- (void)showNear:(NSWindow*)parent {
    if (!_window.isVisible && ![_window setFrameUsingName:@"RabbitEarsMeterBridge"] && parent) {
        const NSRect pf = parent.frame;
        [_window setFrameOrigin:NSMakePoint(NSMinX(pf) + 40, NSMinY(pf) + 40)];
    }
    [self layout];
    [_window makeKeyAndOrderFront:nil];
}

- (void)close { [_window orderOut:nil]; }
- (BOOL)isVisible { return _window.isVisible; }

- (BOOL)windowShouldClose:(NSWindow*)__unused w {
    [_window orderOut:nil];
    if (self.onClose) self.onClose();
    return NO;  // kept for a later re-open
}

- (void)relabelForLanguageChange {
    _window.title = Tr(StringId::MeterBridgeTitle);
    for (int k = 0; k < 4; ++k) {
        _labels[k].attributedStringValue = meterLabelText(k);
        [_meters[k] relabelForLanguageChange];
    }
    _tankLabel.attributedStringValue = meterLabelText(4);
}

@end
