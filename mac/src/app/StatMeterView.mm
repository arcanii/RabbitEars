// SPDX-License-Identifier: GPL-3.0-or-later
// See StatMeterView.h.
#import "StatMeterView.h"

#include <algorithm>
#include <cmath>

namespace {
// Resolve a portable SkinColor (0..255 sRGB) to an NSColor.
NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
}  // namespace

@implementation StatMeterView {
    float     _target;   // most recent pushed level (0..1)
    float     _level;    // smoothed, drawn value
    float     _peak;     // decaying peak-hold cap
    NSString* _readout;
    NSTimer*  _timer;
    // LED colours resolved from the meter palette (see -setPalette:).
    NSColor*  _bgCol;
    NSColor*  _lowCol;
    NSColor*  _midCol;
    NSColor*  _highCol;
    NSColor*  _peakCol;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        _target = _level = _peak = 0.0f;
        _readout = @"";
        [self setPalette:rabbitears::mac::defaultMeterPalette(rabbitears::mac::MeterKind::Bitrate)];
        self.wantsLayer = YES;
        // ~30fps smoothing/decay + redraw. Block-based with a WEAK self so the
        // run-loop-retained timer can't keep the view alive on its own.
        __weak StatMeterView* weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer* t) {
            StatMeterView* s = weakSelf;
            if (s) [s tick];
            else   [t invalidate];
        }];
        _timer.tolerance = 0.01;
    }
    return self;
}

- (void)dealloc { [_timer invalidate]; }

- (void)setLevel:(float)level { _target = std::clamp(level, 0.0f, 1.0f); }

- (void)setReadout:(NSString*)text {
    NSString* t = text ?: @"";
    if (![_readout isEqualToString:t]) { _readout = t; [self setNeedsDisplay:YES]; }
}

- (void)reset {
    _target = _level = _peak = 0.0f;
    _readout = @"";
    [self setNeedsDisplay:YES];
}

- (void)setPalette:(const rabbitears::mac::MeterPalette&)p {
    // bg.inherit == "follow the theme window background" — the mac stand-in is the
    // dark panel colour the meter has always used.
    _bgCol   = p.bg.inherit ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(p.bg);
    _lowCol  = nscolor(p.low);
    _midCol  = nscolor(p.mid);
    _highCol = nscolor(p.high);
    _peakCol = nscolor(p.peak);
    [self setNeedsDisplay:YES];
}

- (void)tick {
    const float pl = _level, pp = _peak;
    _level += (_target > _level ? 0.5f : 0.15f) * (_target - _level);  // fast attack, slow release
    if (_level > _peak) _peak = _level;
    else _peak = std::max(_level, _peak - 0.02f);
    if (_level != pl || _peak != pp) [self setNeedsDisplay:YES];  // idle → stop waking the display
}

- (BOOL)isFlipped { return NO; }

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    [_bgCol setFill];
    NSRectFill(b);

    // Win32-MiniMeter-style LED dot-matrix: small square cells with gaps, lit as a
    // left-to-right bar (whole columns), with a bright peak-hold cap column. Colours
    // come from the meter palette — the ramp runs low -> mid -> high across the bar.
    const CGFloat pad = 2, cell = 3, gap = 1, labelW = 150;

    const CGFloat matrixW = b.size.width - 2 * pad - labelW;
    const int cols = std::max(1, (int)((matrixW + gap) / (cell + gap)));
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gap) / (cell + gap)));
    const int litCols = (int)std::lround(_level * cols);
    const int peakColumn = (int)std::lround(_peak * cols);
    const CGFloat gridH = rows * cell + (rows - 1) * gap;
    const CGFloat y0 = (b.size.height - gridH) / 2.0;
    for (int c = 0; c < cols; ++c) {
        const float frac = cols > 1 ? (float)c / (cols - 1) : 0.0f;
        NSColor* ramp = frac < 0.70f ? _lowCol : (frac < 0.90f ? _midCol : _highCol);
        const BOOL lit = c < litCols;
        const BOOL isPeak = (c == peakColumn && peakColumn > 0);
        [(isPeak ? _peakCol : (lit ? ramp : [ramp colorWithAlphaComponent:0.14])) setFill];
        const CGFloat x = pad + c * (cell + gap);
        for (int r = 0; r < rows; ++r) {
            const CGFloat y = y0 + r * (cell + gap);
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, cell, cell) xRadius:0.8 yRadius:0.8] fill];
        }
    }
    if (_readout.length) {
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:9 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.78 alpha:1.0],
        };
        const NSSize sz = [_readout sizeWithAttributes:attrs];
        [_readout drawAtPoint:NSMakePoint(b.size.width - pad - sz.width, (b.size.height - sz.height) / 2)
               withAttributes:attrs];
    }
}

@end
