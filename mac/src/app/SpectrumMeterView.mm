// SPDX-License-Identifier: GPL-3.0-or-later
// See SpectrumMeterView.h.
#import "SpectrumMeterView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <os/lock.h>

namespace {
constexpr int kMaxBands = 64;  // matches SpectrumTap's clamp
// Resolve a portable SkinColor (0..255 sRGB) to an NSColor.
NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
}  // namespace

@implementation SpectrumMeterView {
    // Latched from the RT audio thread (guarded by _lock); consumed on the UI timer.
    os_unfair_lock _lock;
    float          _target[kMaxBands];
    int            _targetCount;
    BOOL           _haveData;
    BOOL           _hadEnergy;    // any band > floor since last consumeHadEnergy (guarded by _lock)

    // UI-thread animation state.
    float     _level[kMaxBands];  // smoothed, drawn heights
    float     _peak[kMaxBands];   // decaying per-band peak-hold caps
    int       _drawCount;         // bands actually drawn (from the latest push)
    BOOL      _available;         // NO => "grant permission" placeholder
    NSTimer*  _timer;

    // LED colours resolved from the meter palette (see -setPalette:).
    NSColor* _bgCol;
    NSColor* _lowCol;
    NSColor* _midCol;
    NSColor* _highCol;
    NSColor* _peakCol;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        _lock = OS_UNFAIR_LOCK_INIT;
        _targetCount = 0;
        _haveData = NO;
        _hadEnergy = NO;
        _drawCount = 24;
        _available = YES;
        std::memset(_target, 0, sizeof _target);
        std::memset(_level, 0, sizeof _level);
        std::memset(_peak, 0, sizeof _peak);
        self.wantsLayer = YES;
        [self setPalette:rabbitears::mac::defaultMeterPalette(rabbitears::mac::MeterKind::Spectrum)];
        // ~30fps ease/decay + redraw. Block-based with a WEAK self so the
        // run-loop-retained timer can't keep the view alive on its own.
        __weak SpectrumMeterView* weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer* t) {
            SpectrumMeterView* s = weakSelf;
            if (s) [s tick];
            else   [t invalidate];
        }];
        _timer.tolerance = 0.01;
    }
    return self;
}

- (void)dealloc { [_timer invalidate]; }

- (BOOL)isFlipped { return NO; }  // origin bottom-left: bands grow upward

// RT audio thread — keep it to a brief lock + copy.
- (void)pushSpectrum:(const float*)bands count:(int)count {
    if (!bands || count <= 0) return;
    const int n = std::min(count, kMaxBands);
    os_unfair_lock_lock(&_lock);
    std::memcpy(_target, bands, (size_t)n * sizeof(float));
    _targetCount = n;
    _haveData = YES;
    for (int i = 0; i < n; ++i)
        if (bands[i] > 0.02f) { _hadEnergy = YES; break; }  // real audio, not silence
    os_unfair_lock_unlock(&_lock);
}

- (BOOL)consumeHadEnergy {
    os_unfair_lock_lock(&_lock);
    const BOOL e = _hadEnergy;
    _hadEnergy = NO;
    os_unfair_lock_unlock(&_lock);
    return e;
}

- (void)setAvailable:(BOOL)available {
    if (_available == available) return;
    _available = available;
    [self setNeedsDisplay:YES];
}

- (void)reset {
    std::memset(_level, 0, sizeof _level);
    std::memset(_peak, 0, sizeof _peak);
    os_unfair_lock_lock(&_lock);
    _targetCount = 0;
    _haveData = NO;
    _hadEnergy = NO;
    std::memset(_target, 0, sizeof _target);
    os_unfair_lock_unlock(&_lock);
    [self setNeedsDisplay:YES];
}

- (void)setPalette:(const rabbitears::mac::MeterPalette&)p {
    _bgCol   = p.bg.inherit ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(p.bg);
    _lowCol  = nscolor(p.low);
    _midCol  = nscolor(p.mid);
    _highCol = nscolor(p.high);
    _peakCol = nscolor(p.peak);
    [self setNeedsDisplay:YES];
}

- (void)tick {
    if (!_available) return;  // placeholder is static — no easing while unavailable

    float tgt[kMaxBands];
    os_unfair_lock_lock(&_lock);
    const int n = _targetCount;
    if (n > 0) std::memcpy(tgt, _target, (size_t)n * sizeof(float));
    _haveData = NO;
    os_unfair_lock_unlock(&_lock);
    if (n > 0) _drawCount = n;

    BOOL changed = NO;
    for (int b = 0; b < _drawCount; ++b) {
        const float target = (b < n) ? tgt[b] : 0.0f;
        const float pl = _level[b], pp = _peak[b];
        _level[b] += (target > _level[b] ? 0.5f : 0.15f) * (target - _level[b]);  // fast attack / slow release
        if (_level[b] > _peak[b]) _peak[b] = _level[b];
        else _peak[b] = std::max(_level[b], _peak[b] - 0.02f);
        if (_level[b] != pl || _peak[b] != pp) changed = YES;
    }
    if (changed) [self setNeedsDisplay:YES];  // idle -> stop waking the display
}

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    [_bgCol setFill];
    NSRectFill(b);

    if (!_available) {
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:9 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.62 alpha:1.0],
        };
        NSString* msg = @"No audio — check Settings ▸ Privacy ▸ Audio";  // kept short for the ~300px strip
        const NSSize sz = [msg sizeWithAttributes:attrs];
        [msg drawAtPoint:NSMakePoint(std::max(4.0, (b.size.width - sz.width) / 2.0),
                                     (b.size.height - sz.height) / 2.0)
          withAttributes:attrs];
        return;
    }

    // Vertical LED columns, one per band (low->high, left to right). Each column's
    // ramp runs low (bottom) -> mid -> high (top); the per-band peak-hold cap is a
    // bright cell. Columns fill the width; cells are a fixed square-ish LED height.
    const CGFloat pad = 2, cellH = 3, gapY = 1, gapX = 1;
    const int n = std::max(1, _drawCount);
    const CGFloat colStride = (b.size.width - 2 * pad) / n;
    const CGFloat cellW = std::max(2.0, colStride - gapX);
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gapY) / (cellH + gapY)));
    for (int bd = 0; bd < n; ++bd) {
        const CGFloat x = pad + bd * colStride;
        const int litRows = (int)std::lround(_level[bd] * rows);
        const int peakRow = (int)std::lround(_peak[bd] * rows);
        for (int r = 0; r < rows; ++r) {
            const float frac = rows > 1 ? (float)r / (rows - 1) : 0.0f;  // 0 bottom .. 1 top
            NSColor* ramp = frac < 0.70f ? _lowCol : (frac < 0.90f ? _midCol : _highCol);
            const BOOL lit = r < litRows;
            const BOOL isPeak = (r == peakRow && peakRow > 0);
            [(isPeak ? _peakCol : (lit ? ramp : [ramp colorWithAlphaComponent:0.12])) setFill];
            const CGFloat y = pad + r * (cellH + gapY);
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, cellW, cellH) xRadius:0.8 yRadius:0.8] fill];
        }
    }
}

@end
