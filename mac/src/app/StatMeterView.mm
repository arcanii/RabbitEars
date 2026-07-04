// SPDX-License-Identifier: GPL-3.0-or-later
// See StatMeterView.h.
#import "StatMeterView.h"

#include <algorithm>
#include <cmath>

@implementation StatMeterView {
    float     _target;   // most recent pushed level (0..1)
    float     _level;    // smoothed, drawn value
    float     _peak;     // decaying peak-hold cap
    NSString* _readout;
    NSTimer*  _timer;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        _target = _level = _peak = 0.0f;
        _readout = @"";
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
    [[NSColor colorWithWhite:0.08 alpha:1.0] setFill];
    NSRectFill(b);

    // Win32-MiniMeter-style LED dot-matrix: small square cells with gaps, lit as a
    // left-to-right bar (whole columns), with a brighter peak-hold column.
    const CGFloat pad = 2, cell = 5, gap = 2, labelW = 168;
    NSColor* green = [NSColor colorWithRed:0.25 green:0.85 blue:0.40 alpha:1.0];
    NSColor* amber = [NSColor colorWithRed:0.95 green:0.80 blue:0.20 alpha:1.0];
    NSColor* red   = [NSColor colorWithRed:0.95 green:0.30 blue:0.25 alpha:1.0];

    const CGFloat matrixW = b.size.width - 2 * pad - labelW;
    const int cols = std::max(1, (int)((matrixW + gap) / (cell + gap)));
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gap) / (cell + gap)));
    const int litCols = (int)std::lround(_level * cols);
    const int peakCol = (int)std::lround(_peak * cols);
    const CGFloat gridH = rows * cell + (rows - 1) * gap;
    const CGFloat y0 = (b.size.height - gridH) / 2.0;
    for (int c = 0; c < cols; ++c) {
        const float frac = cols > 1 ? (float)c / (cols - 1) : 0.0f;
        NSColor* on = frac < 0.70f ? green : (frac < 0.90f ? amber : red);
        const BOOL lit = c < litCols;
        const BOOL isPeak = (c == peakCol && peakCol > 0);
        [(lit || isPeak ? on : [on colorWithAlphaComponent:0.14]) setFill];
        const CGFloat x = pad + c * (cell + gap);
        for (int r = 0; r < rows; ++r) {
            const CGFloat y = y0 + r * (cell + gap);
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, cell, cell) xRadius:0.8 yRadius:0.8] fill];
        }
    }
    if (_readout.length) {
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.78 alpha:1.0],
        };
        const NSSize sz = [_readout sizeWithAttributes:attrs];
        [_readout drawAtPoint:NSMakePoint(b.size.width - pad - sz.width, (b.size.height - sz.height) / 2)
               withAttributes:attrs];
    }
}

@end
