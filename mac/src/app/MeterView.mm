// SPDX-License-Identifier: GPL-3.0-or-later
// See MeterView.h.
#import "MeterView.h"

#include <algorithm>
#include <atomic>
#include <cmath>

@implementation MeterView {
    std::atomic<float> _target;  // most recent pushed level (written off-thread)
    float              _level;   // smoothed, drawn value
    float              _peak;    // peak-hold marker
    NSTimer*           _timer;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        _target = 0.0f;
        _level = _peak = 0.0f;
        self.wantsLayer = YES;
        // ~30fps smoothing/decay + redraw on the main run loop. Block-based with a
        // WEAK self so the run-loop-retained timer doesn't keep the view alive
        // (a target-based timer would leak the view + never stop).
        __weak MeterView* weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer* t) {
            MeterView* s = weakSelf;
            if (s) [s tick];
            else   [t invalidate];  // view is gone — stop firing
        }];
        _timer.tolerance = 0.01;
    }
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (void)pushLevel:(float)level {
    _target.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

- (void)reset {
    _target.store(0.0f, std::memory_order_relaxed);
    _level = _peak = 0.0f;
    [self setNeedsDisplay:YES];
}

- (void)tick {
    const float t = _target.load(std::memory_order_relaxed);
    const float prevLevel = _level, prevPeak = _peak;
    // Fast attack, slow release — the classic meter ballistics.
    _level += (t > _level ? 0.55f : 0.14f) * (t - _level);
    if (_level > _peak) _peak = _level;
    else _peak = std::max(_level, _peak - 0.012f);  // peak-hold decays gently
    // Only redraw when something moved. At rest (silence / stopped) the values
    // settle to a fixed point, so we stop waking the display every frame.
    if (_level != prevLevel || _peak != prevPeak) [self setNeedsDisplay:YES];
}

- (BOOL)isFlipped { return NO; }

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    [[NSColor colorWithWhite:0.06 alpha:1.0] setFill];
    NSRectFill(b);

    const int segs = 24;
    const CGFloat pad = 3, gap = 2;
    const CGFloat w = (b.size.width - 2 * pad - (segs - 1) * gap) / segs;
    const CGFloat h = b.size.height - 2 * pad;
    if (w <= 0 || h <= 0) return;

    const int lit = (int)std::lround(_level * segs);
    const int peakSeg = (int)std::lround(_peak * segs);
    for (int i = 0; i < segs; ++i) {
        const CGFloat x = pad + i * (w + gap);
        NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, pad, w, h)
                                                          xRadius:1.5 yRadius:1.5];
        const float frac = (float)i / (segs - 1);
        NSColor* on = frac < 0.66f ? [NSColor colorWithRed:0.25 green:0.85 blue:0.35 alpha:1.0]
                    : frac < 0.85f ? [NSColor colorWithRed:0.95 green:0.80 blue:0.20 alpha:1.0]
                                   : [NSColor colorWithRed:0.95 green:0.28 blue:0.24 alpha:1.0];
        const bool isOn = i < lit;
        const bool isPeak = (i == peakSeg && peakSeg > 0);
        if (isOn || isPeak) [(isPeak ? [on colorWithAlphaComponent:0.95] : on) setFill];
        else                [[on colorWithAlphaComponent:0.12] setFill];  // dim "off" LED
        [p fill];
    }
}

@end
