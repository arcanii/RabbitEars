// SPDX-License-Identifier: GPL-3.0-or-later
// See MeterView.h.
#import "MeterView.h"
#import "Tr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <os/lock.h>

using namespace rabbitears::mac;
using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

namespace {
constexpr int kMaxBands = 64;
constexpr int kHist     = 96;  // bitrate history depth
NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
inline float mix(float a, float b, float t) { return a + (b - a) * t; }  // t in 0..1
}  // namespace

@implementation MeterView {
    MeterKind  _kind;
    MeterStyle _style;
    float      _gain;       // tuning.sensitivity (0.5 -> unity gain)
    float      _glow;       // tuning.glow — Tube/Scope bloom radius
    float      _smoothing;  // tuning.smoothing — easing inertia
    float      _peakHold;   // tuning.peakHold — spectrum peak linger
    float      _breathing;  // tuning.breathing — bitrate ceiling ebb
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
        _glow = _smoothing = _peakHold = _breathing = 0.5f;
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
    _gain      = std::clamp(cfg.tuning.sensitivity * 2.0f, 0.05f, 4.0f);  // 0.5 knob == unity
    _glow      = std::clamp(cfg.tuning.glow, 0.0f, 1.0f);
    _smoothing = std::clamp(cfg.tuning.smoothing, 0.0f, 1.0f);
    _peakHold  = std::clamp(cfg.tuning.peakHold, 0.0f, 1.0f);
    _breathing = std::clamp(cfg.tuning.breathing, 0.0f, 1.0f);
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
    _histMax = std::max(bytesPerSec, _histMax * mix(0.99f, 0.95f, _breathing));  // ceiling; 0.5 -> 0.97
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
    // The strip is too narrow for the real instruction — put it in the tooltip.
    self.toolTip = available ? nil : Tr(StringId::MacMeterViewSpectrumUnavailableTooltip);
    [self setNeedsDisplay:YES];
}

// Live language switch: the drawn "needs permission" text re-reads Tr() on redraw, but the cached
// hover toolTip does not — re-apply it (matching the current availability) + repaint.
- (void)relabelForLanguageChange {
    self.toolTip = _available ? nil : Tr(StringId::MacMeterViewSpectrumUnavailableTooltip);
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
            const float attack = mix(0.76f, 0.24f, _smoothing);       // 0.5 -> 0.50 (pre-tuning)
            const float decay  = mix(0.24f, 0.06f, _smoothing);       // 0.5 -> 0.15
            const float peakDecay = mix(0.036f, 0.004f, _peakHold);   // 0.5 -> 0.02
            for (int b = 0; b < _drawBands; ++b) {
                const float target = std::clamp((b < n ? tgt[b] : 0.0f) * _gain, 0.0f, 1.0f);
                const float pl = _specLevel[b], pp = _specPeak[b];
                _specLevel[b] += (target > _specLevel[b] ? attack : decay) * (target - _specLevel[b]);
                if (_specLevel[b] > _specPeak[b]) _specPeak[b] = _specLevel[b];
                else _specPeak[b] = std::max(_specLevel[b], _specPeak[b] - peakDecay);
                if (_specLevel[b] != pl || _specPeak[b] != pp) changed = YES;
            }
            break;
        }
        case MeterKind::Signal: {
            const float e = mix(0.38f, 0.12f, _smoothing);  // 0.5 -> 0.25 (pre-tuning)
            const float ps = _sigStrength, pt = _sigTrouble;
            _sigStrength += e * (_sigStrengthT - _sigStrength);
            _sigTrouble  += e * (_sigTroubleT - _sigTrouble);
            if (_sigStrength != ps || _sigTrouble != pt) changed = YES;
            break;
        }
        case MeterKind::Frames: {
            const float pf = _fps, pl = _flare;
            _fps += mix(0.45f, 0.15f, _smoothing) * (_fpsT - _fps);  // 0.5 -> 0.30 (pre-tuning)
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
// Unlit cell per style: LED/Tube/Scope = solid off; LCD = a faint 16% ghost of the lit colour.
- (NSColor*)unlitFor:(NSColor*)lit {
    if (_style == MeterStyle::Lcd) return [_off blendedColorWithFraction:0.16 ofColor:lit];
    return _off;
}

// One dot-matrix cell honouring the style. LED/LCD fill flat (LCD's unlit is ghosted by
// -unlitFor:); Tube blooms a lit cell with a translucent halo sized by the glow knob, so
// overlapping halos read as a warm valve glow.
- (void)fillCell:(NSRect)r color:(NSColor*)lit lit:(BOOL)on {
    if (!on) { [[self unlitFor:lit] setFill]; NSRectFill(r); return; }
    if (_style == MeterStyle::Tube && _glow > 0.01f) {
        const CGFloat halo = 1.0 + _glow * 3.0;
        [[lit colorWithAlphaComponent:0.20f + 0.30f * _glow] setFill];
        NSRectFill(NSInsetRect(r, -halo, -halo));
    }
    [lit setFill];
    NSRectFill(r);
}

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    [_bg setFill];
    NSRectFill(b);
    [[NSColor colorWithWhite:0.22 alpha:1.0] set];
    NSFrameRectWithWidth(b, 1);
    if (_kind == MeterKind::Spectrum && !_available) { [self drawUnavailable:b]; return; }
    if (_style == MeterStyle::Scope) { [self drawScope:b]; return; }
    switch (_kind) {
        case MeterKind::Spectrum: [self drawSpectrum:b]; break;
        case MeterKind::Signal:   [self drawSignal:b]; break;
        case MeterKind::Bitrate:  [self drawBitrate:b]; break;
        case MeterKind::Frames:   [self drawFrames:b]; break;
    }
}

- (void)drawUnavailable:(NSRect)b {
    // The Spectrum strip is only ~180pt wide: the previous 43-character message was wider
    // than the view and ran off both edges. Use a short message and shrink the font until it
    // fits; the full instruction lives in the tooltip (see -setAvailable:).
    NSColor* fg = [NSColor colorWithWhite:0.62 alpha:1.0];
    NSString* msg = Tr(StringId::MacMeterViewSpectrumNeedsPermission);
    for (CGFloat pt = 9.0;; pt -= 0.5) {
        NSDictionary* attrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:pt weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: fg,
        };
        const NSSize sz = [msg sizeWithAttributes:attrs];
        if (sz.width <= b.size.width - 6 || pt <= 6.5) {
            [msg drawAtPoint:NSMakePoint(std::max(3.0, (b.size.width - sz.width) / 2.0),
                                         (b.size.height - sz.height) / 2.0)
              withAttributes:attrs];
            return;
        }
    }
}

- (void)drawSpectrum:(NSRect)b {
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
            const BOOL isPeak = (r == peakRow && peakRow > 0);
            const NSRect cell = NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH);
            if (isPeak) [self fillCell:cell color:_peak lit:YES];
            else        [self fillCell:cell color:rc lit:(r < litRows)];
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
        for (int r = 0; r < rows; ++r)
            [self fillCell:NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH) color:base lit:barLit];
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
            NSColor* litc = [_accent blendedColorWithFraction:rf * 0.5f ofColor:_peak];
            [self fillCell:NSMakeRect(x, pad + r * (cellH + gapY), std::max(1.0, colW - 1), cellH)
                     color:litc lit:(r < litRows)];
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
        for (int r = 0; r < rows; ++r)
            [self fillCell:NSMakeRect(x, pad + r * (cellH + gapY), cellW, cellH) color:base lit:on];
    }
}

// ---- Scope style: a phosphor oscilloscope trace of each kind's level series -------
- (void)drawScope:(NSRect)b {
    float series[kMaxBands];
    int n = 0;
    NSColor* trace = _accent;
    switch (_kind) {
        case MeterKind::Spectrum:
            n = std::min(kMaxBands, std::max(1, _drawBands));
            for (int i = 0; i < n; ++i) series[i] = std::clamp(_specLevel[i], 0.0f, 1.0f);
            break;
        case MeterKind::Bitrate: {
            const int show = std::min(kHist, 48);
            for (int i = 0; i < show; ++i) {  // oldest -> newest, left -> right
                const int idx = ((_histHead - show + i) % kHist + kHist) % kHist;
                series[i] = std::clamp(_hist[idx] * _gain, 0.0f, 1.0f);
            }
            n = show;
            break;
        }
        case MeterKind::Signal: {
            const float s = std::clamp(_sigStrength * _gain, 0.0f, 1.0f);
            series[0] = series[1] = s;
            n = 2;
            trace = s > 0.6f ? _low : (s > 0.3f ? _mid : _high);
            trace = [trace blendedColorWithFraction:std::clamp(_sigTrouble * 0.6f, 0.0f, 1.0f) ofColor:_high];
            break;
        }
        case MeterKind::Frames: {
            const float s = std::clamp(_fps / 60.0f * _gain, 0.0f, 1.0f);
            series[0] = series[1] = s;
            n = 2;
            const int fps = (int)std::lround(_fps);
            trace = fps >= 24 ? _low : (fps >= 15 ? _mid : _high);
            trace = [trace blendedColorWithFraction:std::clamp(_flare, 0.0f, 1.0f) ofColor:_high];
            break;
        }
    }
    [self strokeScope:series count:n in:b color:trace];
}

// A dim filled area under a glowing polyline of the level series; the glow knob sets
// the phosphor bloom radius.
- (void)strokeScope:(const float*)v count:(int)n in:(NSRect)b color:(NSColor*)c {
    if (n < 1) return;
    const CGFloat pad = 3;
    const CGFloat w = std::max(1.0, b.size.width - 2 * pad);
    const CGFloat h = std::max(1.0, b.size.height - 2 * pad);
    NSBezierPath* line = [NSBezierPath bezierPath];
    for (int i = 0; i < n; ++i) {
        const CGFloat x = pad + (n == 1 ? w * 0.5 : w * i / (n - 1));
        const CGFloat y = pad + std::clamp(v[i], 0.0f, 1.0f) * h;
        if (i == 0) [line moveToPoint:NSMakePoint(x, y)];
        else        [line lineToPoint:NSMakePoint(x, y)];
    }
    NSBezierPath* area = [line copy];
    [area lineToPoint:NSMakePoint(pad + w, pad)];
    [area lineToPoint:NSMakePoint(pad, pad)];
    [area closePath];
    [[c colorWithAlphaComponent:0.16] setFill];
    [area fill];

    [NSGraphicsContext saveGraphicsState];
    if (_glow > 0.01f) {
        NSShadow* sh = [[NSShadow alloc] init];
        sh.shadowBlurRadius = 1.5 + _glow * 6.0;
        sh.shadowColor = c;
        sh.shadowOffset = NSZeroSize;
        [sh set];
    }
    line.lineWidth = 1.5;
    line.lineJoinStyle = NSLineJoinStyleRound;
    [[_peak blendedColorWithFraction:0.35 ofColor:c] setStroke];
    [line stroke];
    [NSGraphicsContext restoreGraphicsState];
}

@end
