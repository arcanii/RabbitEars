// SPDX-License-Identifier: GPL-3.0-or-later
// See MeterView.h.
#import "MeterView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <os/lock.h>

using namespace rabbitears::mac;

namespace {
constexpr int kMaxBands = 64;
constexpr int kHist     = 96;  // bitrate history depth
NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
}  // namespace

@implementation MeterView {
    MeterKind  _kind;
    MeterStyle _style;
    float      _gain;   // from tuning.sensitivity (0.5 -> 1.0 unity)
    NSTimer*   _timer;

    NSColor *_bg, *_off, *_low, *_mid, *_high, *_accent, *_peak;

    // Spectrum — latched from the RT audio thread under _lock, eased on the timer.
    os_unfair_lock _lock;
    float _specTarget[kMaxBands];
    int   _specCount;
    BOOL  _haveData, _hadEnergy, _available;
    float _specLevel[kMaxBands], _specPeak[kMaxBands];
    int   _drawBands;

    // Signal (UI thread)
    float _sigStrengthT, _sigTroubleT, _sigStrength, _sigTrouble;
    // Bitrate (UI thread) — a ring of normalized 0..1 samples
    float  _hist[kHist];
    int    _histHead;
    double _histMax;
    // Frames (UI thread)
    float _fpsT, _fps, _flare;
}

- (instancetype)initWithKind:(MeterKind)kind {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 120, 24)])) {
        _kind = kind;
        _style = MeterStyle::Led;
        _gain = 1.0f;
        _lock = OS_UNFAIR_LOCK_INIT;
        _available = YES;
        _drawBands = 24;
        _histMax = 1.0;
        std::memset(_specTarget, 0, sizeof _specTarget);
        std::memset(_specLevel, 0, sizeof _specLevel);
        std::memset(_specPeak, 0, sizeof _specPeak);
        std::memset(_hist, 0, sizeof _hist);
        self.wantsLayer = YES;
        MeterConfig def{};
        def.palette = defaultMeterPalette(kind);
        [self setConfig:def];
        __weak MeterView* weak = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 repeats:YES block:^(NSTimer* t) {
            MeterView* s = weak;
            if (s) [s tick];
            else   [t invalidate];
        }];
        _timer.tolerance = 0.01;
    }
    return self;
}

- (void)dealloc { [_timer invalidate]; }
- (MeterKind)kind { return _kind; }
- (BOOL)isFlipped { return NO; }

- (void)setConfig:(const MeterConfig&)cfg {
    _style = cfg.style;
    _gain = std::clamp(cfg.tuning.sensitivity * 2.0f, 0.05f, 4.0f);  // 0.5 knob == unity
    const MeterPalette& p = cfg.palette;
    _bg     = p.bg.inherit ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(p.bg);
    _off    = nscolor(p.off);
    _low    = nscolor(p.low);
    _mid    = nscolor(p.mid);
    _high   = nscolor(p.high);
    _accent = nscolor(p.accent);
    _peak   = nscolor(p.peak);
    [self setNeedsDisplay:YES];
}

// ---- data feeds -------------------------------------------------------------
- (void)pushSpectrum:(const float*)bands count:(int)count {
    if (!bands || count <= 0) return;
    const int n = std::min(count, kMaxBands);
    os_unfair_lock_lock(&_lock);
    std::memcpy(_specTarget, bands, (size_t)n * sizeof(float));
    _specCount = n;
    _haveData = YES;
    for (int i = 0; i < n; ++i)
        if (bands[i] > 0.02f) { _hadEnergy = YES; break; }
    os_unfair_lock_unlock(&_lock);
}

- (void)setSignal:(float)strength trouble:(float)trouble {
    _sigStrengthT = std::clamp(strength, 0.0f, 1.0f);
    _sigTroubleT  = std::clamp(trouble, 0.0f, 1.0f);
}

- (void)pushBitrate:(double)bytesPerSec {
    _histMax = std::max(bytesPerSec, _histMax * 0.97);  // adaptive ceiling
    const double denom = std::max(_histMax, 1000.0);
    _hist[_histHead] = (float)std::clamp(bytesPerSec / denom, 0.0, 1.0);
    _histHead = (_histHead + 1) % kHist;
    [self setNeedsDisplay:YES];
}

- (void)setFrames:(int)fps dropsDelta:(int)dropsDelta {
    _fpsT = (float)fps;
    if (dropsDelta > 0) _flare = 1.0f;
}

- (void)setAvailable:(BOOL)available {
    if (_available == available) return;
    _available = available;
    [self setNeedsDisplay:YES];
}

- (BOOL)consumeHadEnergy {
    os_unfair_lock_lock(&_lock);
    const BOOL e = _hadEnergy;
    _hadEnergy = NO;
    os_unfair_lock_unlock(&_lock);
    return e;
}

- (void)reset {
    std::memset(_specLevel, 0, sizeof _specLevel);
    std::memset(_specPeak, 0, sizeof _specPeak);
    _sigStrength = _sigTrouble = _sigStrengthT = _sigTroubleT = 0;
    std::memset(_hist, 0, sizeof _hist);
    _histHead = 0;
    _histMax = 1.0;
    _fps = _fpsT = _flare = 0;
    os_unfair_lock_lock(&_lock);
    _specCount = 0;
    _haveData = NO;
    _hadEnergy = NO;
    std::memset(_specTarget, 0, sizeof _specTarget);
    os_unfair_lock_unlock(&_lock);
    [self setNeedsDisplay:YES];
}

- (void)tick {
    BOOL changed = NO;
    switch (_kind) {
        case MeterKind::Spectrum: {
            if (!_available) return;
            float tgt[kMaxBands];
            os_unfair_lock_lock(&_lock);
            const int n = _specCount;
            if (n > 0) std::memcpy(tgt, _specTarget, (size_t)n * sizeof(float));
            _haveData = NO;
            os_unfair_lock_unlock(&_lock);
            if (n > 0) _drawBands = n;
            for (int b = 0; b < _drawBands; ++b) {
                const float target = std::clamp((b < n ? tgt[b] : 0.0f) * _gain, 0.0f, 1.0f);
                const float pl = _specLevel[b], pp = _specPeak[b];
                _specLevel[b] += (target > _specLevel[b] ? 0.5f : 0.15f) * (target - _specLevel[b]);
                if (_specLevel[b] > _specPeak[b]) _specPeak[b] = _specLevel[b];
                else _specPeak[b] = std::max(_specLevel[b], _specPeak[b] - 0.02f);
                if (_specLevel[b] != pl || _specPeak[b] != pp) changed = YES;
            }
            break;
        }
        case MeterKind::Signal: {
            const float ps = _sigStrength, pt = _sigTrouble;
            _sigStrength += 0.25f * (_sigStrengthT - _sigStrength);
            _sigTrouble  += 0.25f * (_sigTroubleT - _sigTrouble);
            if (_sigStrength != ps || _sigTrouble != pt) changed = YES;
            break;
        }
        case MeterKind::Frames: {
            const float pf = _fps, pl = _flare;
            _fps += 0.3f * (_fpsT - _fps);
            _flare = std::max(0.0f, _flare - 0.05f);
            if (_fps != pf || _flare != pl) changed = YES;
            break;
        }
        case MeterKind::Bitrate:
            break;  // push-driven redraw
    }
    if (changed) [self setNeedsDisplay:YES];
}

// ---- rendering --------------------------------------------------------------
- (NSColor*)ramp:(float)frac {
    return frac < 0.60f ? _low : (frac < 0.85f ? _mid : _high);
}
// Unlit cell per style: LED (+ Tube/Scope, still LED for now) = solid off; LCD = a
// faint 16% ghost of the lit colour.
- (NSColor*)unlitFor:(NSColor*)lit {
    if (_style == MeterStyle::Lcd) return [_off blendedColorWithFraction:0.16 ofColor:lit];
    return _off;
}

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    [_bg setFill];
    NSRectFill(b);
    [[NSColor colorWithWhite:0.22 alpha:1.0] set];
    NSFrameRectWithWidth(b, 1);
    switch (_kind) {
        case MeterKind::Spectrum: [self drawSpectrum:b]; break;
        case MeterKind::Signal:   [self drawSignal:b]; break;
        case MeterKind::Bitrate:  [self drawBitrate:b]; break;
        case MeterKind::Frames:   [self drawFrames:b]; break;
    }
}

- (void)drawSpectrum:(NSRect)b {
    if (!_available) {
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:9 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.62 alpha:1.0],
        };
        NSString* msg = @"No audio — check Settings ▸ Privacy ▸ Audio";
        const NSSize sz = [msg sizeWithAttributes:attrs];
        [msg drawAtPoint:NSMakePoint(std::max(4.0, (b.size.width - sz.width) / 2.0),
                                     (b.size.height - sz.height) / 2.0)
          withAttributes:attrs];
        return;
    }
    const CGFloat pad = 2, cellH = 3, gapY = 1, gapX = 1;
    const int n = std::max(1, _drawBands);
    const CGFloat colStride = (b.size.width - 2 * pad) / n;
    const CGFloat cellW = std::max(2.0, colStride - gapX);
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gapY) / (cellH + gapY)));
    for (int bd = 0; bd < n; ++bd) {
        const CGFloat x = pad + bd * colStride;
        const int litRows = (int)std::lround(_specLevel[bd] * rows);
        const int peakRow = (int)std::lround(_specPeak[bd] * rows);
        for (int r = 0; r < rows; ++r) {
            const float frac = rows > 1 ? (float)r / (rows - 1) : 0.0f;
            NSColor* rc = [self ramp:frac];
            const BOOL lit = r < litRows;
            const BOOL isPeak = (r == peakRow && peakRow > 0);
            [(isPeak ? _peak : (lit ? rc : [self unlitFor:rc])) setFill];
            NSRectFill(NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH));
        }
    }
}

- (void)drawSignal:(NSRect)b {
    const CGFloat pad = 2, cellH = 3, gapY = 1, gapX = 2;
    const int bars = 5;
    const CGFloat colStride = (b.size.width - 2 * pad) / bars;
    const CGFloat cellW = std::max(2.0, colStride - gapX);
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gapY) / (cellH + gapY)));
    const float strength = std::clamp(_sigStrength * _gain, 0.0f, 1.0f);
    const int litBars = (int)std::ceil(strength * bars - 0.001f);
    NSColor* base = strength > 0.6f ? _low : (strength > 0.3f ? _mid : _high);
    base = [base blendedColorWithFraction:std::clamp(_sigTrouble * 0.6f, 0.0f, 1.0f) ofColor:_high];
    for (int c = 0; c < bars; ++c) {
        const CGFloat x = pad + c * colStride;
        const BOOL barLit = c < litBars;
        for (int r = 0; r < rows; ++r) {
            [(barLit ? base : [self unlitFor:base]) setFill];
            NSRectFill(NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH));
        }
    }
}

- (void)drawBitrate:(NSRect)b {
    const CGFloat pad = 2, cellH = 3, gapY = 1, colW = 4;
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gapY) / (cellH + gapY)));
    const int cols = std::max(1, (int)((b.size.width - 2 * pad) / colW));
    const int show = std::min(cols, kHist);
    for (int i = 0; i < show; ++i) {  // newest at the right
        const int idx = ((_histHead - 1 - i) % kHist + kHist) % kHist;
        const int litRows = (int)std::lround(std::clamp(_hist[idx] * _gain, 0.0f, 1.0f) * rows);
        const CGFloat x = pad + (cols - 1 - i) * colW;
        for (int r = 0; r < rows; ++r) {
            const float rf = rows > 1 ? (float)r / (rows - 1) : 0.0f;
            NSColor* lit = [_accent blendedColorWithFraction:rf * 0.5f ofColor:_peak];
            const BOOL on = r < litRows;
            [(on ? lit : [self unlitFor:lit]) setFill];
            NSRectFill(NSMakeRect(x, pad + r * (cellH + gapY), std::max(1.0, colW - 1), cellH));
        }
    }
}

- (void)drawFrames:(NSRect)b {
    const CGFloat pad = 2, cellW = 3, gapX = 1, cellH = 3, gapY = 1;
    const int cols = std::max(1, (int)((b.size.width - 2 * pad + gapX) / (cellW + gapX)));
    const int rows = std::max(1, (int)((b.size.height - 2 * pad + gapY) / (cellH + gapY)));
    const int fps = (int)std::lround(_fps);
    const int lit = (int)std::lround(std::clamp(_fps / 60.0f * _gain, 0.0f, 1.0f) * cols);
    NSColor* base = fps >= 24 ? _low : (fps >= 15 ? _mid : _high);
    base = [base blendedColorWithFraction:std::clamp(_flare, 0.0f, 1.0f) ofColor:_high];
    for (int c = 0; c < cols; ++c) {
        const BOOL on = c < lit;
        const CGFloat x = pad + c * (cellW + gapX);
        for (int r = 0; r < rows; ++r) {
            [(on ? base : [self unlitFor:base]) setFill];
            NSRectFill(NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH));
        }
    }
}

@end
