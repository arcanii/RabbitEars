// SPDX-License-Identifier: GPL-3.0-or-later
// See MeterView.h.
#import "MeterView.h"
#import "Tr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <os/lock.h>
#include <vector>

#include "VuDialMac.h"     // the two needle instruments (port of Win32/ui/VuDial)
#include "ui/PhotoCells.h" // the photoreal cell looks' rasteriser (shared with Win32)

using namespace rabbitears::mac;
using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

namespace {
constexpr int kMaxBands = 64;
constexpr int kHist     = 256;  // bitrate history depth (Win32 kBitrateHistory: a tall cell dial fills)
constexpr float kSilenceDbfs = -120.0f;        // Win32 SpectrumTap::kSilenceDbfs
constexpr float kVuReferenceDbfs = -18.0f;     // 0 VU (the EBU alignment level) — Win32 MiniMeter.h
constexpr float kVuSensSpanDb = 12.0f;         // the Sens knob: ±12 dB at its ends
constexpr float kVuAudioSpanDb = 30.0f;        // the audio needle: dB of input the arc spans
constexpr float kVuRedFrom = 0.8f;             // non-audio kinds: the level where the red zone begins
inline float vuReadingOfDbfs(float dbfs, float sensitivity) {
    return dbfs - kVuReferenceDbfs + (sensitivity - 0.5f) * 2.0f * kVuSensSpanDb;
}
COLORREF cref(const rabbitears::SkinColor& c) { return RGB(c.r, c.g, c.b); }
COLORREF lerpCol(COLORREF a, COLORREF b, float t) {  // Win32 MiniMeter.cpp lerpCol
    t = std::clamp(t, 0.0f, 1.0f);
    auto m = [t](int x, int y) { return x + static_cast<int>((y - x) * t); };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}
rabbitears::mac::VuFace vuFaceOf(rabbitears::mac::MeterStyle s) {
    return s == rabbitears::mac::MeterStyle::VuSilver ? rabbitears::mac::VuFace::Silver : rabbitears::mac::VuFace::Backlit;
}
// The mac panel behind a meter when its Bg inherits (the strip's own dark surround).
constexpr COLORREF kInheritPanel = RGB(20, 20, 20);  // == colorWithWhite:0.08
constexpr COLORREF kChromeBorder = RGB(56, 56, 56);  // == colorWithWhite:0.22 (the 1-px frame)
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

    // ---- the needle + photoreal looks --------------------------------------------------------
    MeterPalette _palette;     // the RAW palette (the photo / VU looks read COLORREFs from it)
    float _sensitivity;        // tuning.sensitivity as set (the audio needle takes it in dB)
    // The programme's level, latched from the RT thread under _lock (Win32 hasPendingLevel).
    float _pendingLevelDb;
    BOOL  _hasPendingLevel;
    BOOL  _audioLevelOn;       // a level has been pushed: the audio needle reads it
    int   _audioStale;         // ticks since the last one (5 = the sound stopped)
    float _audioDb, _audioAmp; // the latest level, and its amplitude eased with VU ballistics
    float _vuNeedle, _vuPeak;  // the needle's level (0..1) and the PEAK lamp's glow (0..1)
    std::vector<uint32_t> _px;            // the frame, 0x00RRGGBB top-down, backing pixels
    std::vector<uint32_t> _vuStatic;      // the VU face without its needle (rebuilt on change)
    VuDialLayout _vuLay;
    VuDialSpec   _vuSpec;
    int   _vuW, _vuH;
    float _vuScale;
    PhotoCellCache _photoCache;           // the cells the photo looks shaded (Win32 photoCache)
    __unsafe_unretained MeterView* _mirror;  // the bridge's twin (guarded by _lock for the RT feeds)
    std::vector<PhotoCell> _photoCells;
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
    if (!isPhotoCellLook(cfg.style)) _photoCache = PhotoCellCache();  // dropped when it leaves those looks
    _style = cfg.style;
    _palette = cfg.palette;
    _sensitivity = std::isfinite(cfg.tuning.sensitivity) ? std::clamp(cfg.tuning.sensitivity, 0.0f, 1.0f) : 0.5f;
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
    if (_mirror) [_mirror pushSpectrum:bands count:count];  // under THIS lock (see -setMirror:)
    std::memcpy(_specTarget, bands, (size_t)n * sizeof(float));
    _specCount = n;
    _haveData = YES;
    for (int i = 0; i < n; ++i)
        if (bands[i] > 0.02f) { _hadEnergy = YES; break; }
    os_unfair_lock_unlock(&_lock);
}

- (void)pushLevel:(float)dbfs {
    if (!std::isfinite(dbfs)) dbfs = kSilenceDbfs;
    os_unfair_lock_lock(&_lock);
    if (_mirror) [_mirror pushLevel:dbfs];
    _pendingLevelDb = _hasPendingLevel ? std::max(_pendingLevelDb, dbfs) : dbfs;
    _hasPendingLevel = YES;
    os_unfair_lock_unlock(&_lock);
}

- (void)setSignal:(float)strength trouble:(float)trouble {
    [_mirror setSignal:strength trouble:trouble];
    _sigStrengthT = std::clamp(strength, 0.0f, 1.0f);
    _sigTroubleT  = std::clamp(trouble, 0.0f, 1.0f);
}

- (void)pushBitrate:(double)bytesPerSec {
    [_mirror pushBitrate:bytesPerSec];
    _histMax = std::max(bytesPerSec, _histMax * mix(0.99f, 0.95f, _breathing));  // ceiling; 0.5 -> 0.97
    const double denom = std::max(_histMax, 1000.0);
    _hist[_histHead] = (float)std::clamp(bytesPerSec / denom, 0.0, 1.0);
    _histHead = (_histHead + 1) % kHist;
    [self setNeedsDisplay:YES];
}

- (void)setFrames:(int)fps dropsDelta:(int)dropsDelta {
    [_mirror setFrames:fps dropsDelta:dropsDelta];
    _fpsT = (float)fps;
    if (dropsDelta > 0) _flare = 1.0f;
}

- (void)setAvailable:(BOOL)available {
    [_mirror setAvailable:available];
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

- (void)setMirror:(MeterView*)mirror {
    if (mirror == self) mirror = nil;
    if (mirror) {  // seed the twin with what this meter already shows
        std::memcpy(mirror->_hist, _hist, sizeof _hist);
        mirror->_histHead = _histHead;
        mirror->_histMax = _histMax;
        std::memcpy(mirror->_specLevel, _specLevel, sizeof _specLevel);
        std::memcpy(mirror->_specPeak, _specPeak, sizeof _specPeak);
        mirror->_drawBands = _drawBands;
        mirror->_sigStrength = _sigStrength;
        mirror->_sigStrengthT = _sigStrengthT;
        mirror->_fps = _fps;
        mirror->_fpsT = _fpsT;
        mirror->_vuNeedle = _vuNeedle;
        [mirror setAvailable:_available];
        [mirror setNeedsDisplay:YES];
    }
    os_unfair_lock_lock(&_lock);  // waits out an RT forward in flight
    _mirror = mirror;
    os_unfair_lock_unlock(&_lock);
}

- (BOOL)consumeHadEnergy {
    os_unfair_lock_lock(&_lock);
    const BOOL e = _hadEnergy;
    _hadEnergy = NO;
    os_unfair_lock_unlock(&_lock);
    return e;
}

- (void)reset {
    [_mirror reset];
    std::memset(_specLevel, 0, sizeof _specLevel);
    std::memset(_specPeak, 0, sizeof _specPeak);
    _sigStrength = _sigTrouble = _sigStrengthT = _sigTroubleT = 0;
    std::memset(_hist, 0, sizeof _hist);
    _histHead = 0;
    _histMax = 1.0;
    _fps = _fpsT = _flare = 0;
    _vuNeedle = _vuPeak = 0;
    _audioAmp = 0;
    _audioDb = kSilenceDbfs;
    os_unfair_lock_lock(&_lock);
    _hasPendingLevel = NO;
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
    if ([self tickNeedle]) changed = YES;
    if (changed) [self setNeedsDisplay:YES];
}

// The 0..1 quantity the needle shows, as ONE number (Win32 scalarLevel). Sensitivity applies once:
// mac's band levels already carry the gain (the tick multiplies the target), the others take it here;
// the audio needle takes the Sens knob in dB instead.
- (float)scalarLevel {
    float raw = 0.0f;
    switch (_kind) {
        case MeterKind::Spectrum: {
            if (_audioLevelOn) {
                const float db = 20.0f * std::log10(std::max(_audioAmp, 1e-6f));
                const float vu = vuReadingOfDbfs(db, _sensitivity);
                return std::clamp(1.0f - (vuDialFullScaleDb(vuFaceOf(_style)) - vu) / kVuAudioSpanDb, 0.0f, 1.0f);
            }
            const int n = std::clamp(_drawBands, 1, kMaxBands);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += std::clamp(_specLevel[i], 0.0f, 1.0f);
            return std::clamp(sum / (float)n, 0.0f, 1.0f);
        }
        case MeterKind::Bitrate: raw = _hist[((_histHead - 1) % kHist + kHist) % kHist]; break;
        case MeterKind::Signal: raw = _sigStrength; break;
        case MeterKind::Frames: raw = _fps / 60.0f; break;
    }
    return std::clamp(raw * _gain, 0.0f, 1.0f);
}

// Where a level puts the needle on the printed scale (Win32 vuPosition): the audio kind is already
// in dB, so linearly over the top kVuAudioSpanDb; the others (not dB quantities) keep the linear law,
// the red zone the top fifth.
- (float)vuPosition:(float)level face:(VuFace)face {
    if (!std::isfinite(level)) level = 0.0f;
    level = std::clamp(level, 0.0f, 1.0f);
    const float full = vuDialFullScaleDb(face);
    if (_kind == MeterKind::Spectrum) return vuDialPositionOfDb(face, full - (1.0f - level) * kVuAudioSpanDb);
    const float f0 = vuDialPositionOfDb(face, 0.0f), fEnd = vuDialPositionOfDb(face, full);
    return level <= kVuRedFrom ? level / kVuRedFrom * f0
                               : f0 + (level - kVuRedFrom) / (1.0f - kVuRedFrom) * (fEnd - f0);
}

// VU needle ballistics, integrated EVERY tick whatever the look (switching to a VU shows a needle
// already tracking). A real movement settles 99 % in ~300 ms, symmetric: ~0.40 a 33-ms tick, the
// Smoothing knob scaling it. The audio needle eases the programme's AMPLITUDE, then shows it in dB.
// PEAK lights whenever the needle is in the red zone, fading over ~1/4 s. (Win32 onTick.) Returns
// whether a needle look needs a redraw.
- (BOOL)tickNeedle {
    if (_kind == MeterKind::Spectrum) {
        os_unfair_lock_lock(&_lock);
        if (_hasPendingLevel) {
            _audioDb = _pendingLevelDb;
            _audioLevelOn = YES;
            _audioStale = 0;
            _hasPendingLevel = NO;
        } else if (_audioLevelOn) {
            if (_audioStale < 5) ++_audioStale;
            if (_audioStale >= 5) _audioDb = kSilenceDbfs;  // the sound stopped: rest
        }
        os_unfair_lock_unlock(&_lock);
    }
    const float vuCoef = std::clamp(0.40f * (1.35f - _smoothing * 0.70f), 0.06f, 0.75f);
    const bool audioNeedle = _kind == MeterKind::Spectrum && _audioLevelOn;
    if (audioNeedle) {
        const float target = std::pow(10.0f, _audioDb / 20.0f);
        _audioAmp += (target - _audioAmp) * vuCoef;
        if (!std::isfinite(_audioAmp)) _audioAmp = 0.0f;
    }
    const float before = _vuNeedle, peakBefore = _vuPeak;
    const float level = [self scalarLevel];
    if (audioNeedle) _vuNeedle = level;
    else _vuNeedle += (level - _vuNeedle) * vuCoef;
    if (!std::isfinite(_vuNeedle)) _vuNeedle = 0.0f;
    const VuFace face = vuFaceOf(_style);
    const bool inRed = [self vuPosition:_vuNeedle face:face] >= vuDialPositionOfDb(face, 0.0f);
    _vuPeak = inRed ? 1.0f : _vuPeak * 0.80f;
    if (_vuPeak < 0.01f) _vuPeak = 0.0f;
    return isVuLook(_style) && (std::fabs(_vuNeedle - before) > 1e-4f || _vuPeak != peakBefore);
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
    if (isVuLook(_style) || isPhotoCellLook(_style)) { [self drawPixelLook:b]; return; }
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

// ---- the pixel looks: the VU instruments and the photoreal cells ---------------------------------
// Both are drawn into a backing-resolution 0x00RRGGBB buffer, as Win32 draws them into its DIB, then
// blitted: the panel, the 1-px frame, then the dial inside the meter's chrome band (Win32 onPaint).

// The colour roles as drawn (Win32 meterDrawnPalette, the part that applies on mac's dark panel): on
// the needle looks the STOCK accent means the face's own black needle.
- (MeterPalette)drawnPalette {
    MeterPalette d = _palette;
    if (isVuLook(_style)) {
        const MeterPalette stock = defaultMeterPalette(MeterKind::Spectrum);
        if (d.accent.r == stock.accent.r && d.accent.g == stock.accent.g && d.accent.b == stock.accent.b) {
            const COLORREF n = vuDialNeedleColour(vuFaceOf(_style));
            d.accent = SkinColor{GetRValue(n), GetGValue(n), GetBValue(n)};
        }
    }
    return d;
}

- (void)drawPixelLook:(NSRect)b {
    const NSRect pb = [self convertRectToBacking:b];
    const int W = (int)std::lround(pb.size.width), H = (int)std::lround(pb.size.height);
    if (W < 4 || H < 4) return;
    const float scale = (float)(pb.size.width / std::max<CGFloat>(1, b.size.width));
    const UINT dpi = (UINT)std::lround(96.0f * scale);
    // The VU looks keep the strip's panel round the instrument (its Bg is the LAMP, not a panel).
    const COLORREF panel = (isVuLook(_style) || _palette.bg.inherit) ? kInheritPanel : cref(_palette.bg);
    _px.assign((size_t)W * (size_t)H, ((uint32_t)GetRValue(panel) << 16) | ((uint32_t)GetGValue(panel) << 8) |
                                          (uint32_t)GetBValue(panel));
    const uint32_t border = ((uint32_t)GetRValue(kChromeBorder) << 16) | ((uint32_t)GetGValue(kChromeBorder) << 8) |
                            (uint32_t)GetBValue(kChromeBorder);
    for (int x = 0; x < W; ++x) { _px[(size_t)x] = border; _px[(size_t)(H - 1) * W + x] = border; }
    for (int y = 0; y < H; ++y) { _px[(size_t)y * W] = border; _px[(size_t)y * W + W - 1] = border; }
    const MeterPalette dp = [self drawnPalette];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef g = CGBitmapContextCreate(_px.data(), (size_t)W, (size_t)H, 8, (size_t)W * 4, cs,
                                           kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (g) {
        CGContextTranslateCTM(g, 0, H);  // y down, as the Win32 renderers draw
        CGContextScaleCTM(g, 1, -1);
    }
    if (isVuLook(_style)) {
        const int inset = MulDiv(2, (int)dpi, 96);  // meterChromePx
        const int w = W - 2 * inset, h = H - 2 * inset;
        if (w >= 8 && h >= 8) {
            VuDialSpec spec;
            spec.face = vuFaceOf(_style);
            spec.lamp = _palette.bg.inherit ? CLR_INVALID : cref(_palette.bg);
            spec.redZone = cref(dp.high);
            if (_vuStatic.empty() || _vuW != w || _vuH != h || _vuScale != scale || !(_vuSpec == spec)) {
                buildVuDialStatic(spec, w, h, scale, _vuStatic, _vuLay);
                _vuSpec = spec;
                _vuW = w;
                _vuH = h;
                _vuScale = scale;
            }
            if (_vuStatic.size() == (size_t)w * (size_t)h) {
                for (int y = 0; y < h; ++y)
                    std::memcpy(_px.data() + (size_t)(inset + y) * W + inset, _vuStatic.data() + (size_t)y * w,
                                (size_t)w * 4);
                drawVuDialNeedle(g, inset, inset, _vuLay, [self vuPosition:_vuNeedle face:spec.face], _vuPeak,
                                 cref(dp.accent));
            }
        }
    } else {
        [self drawPhotoCellsW:W h:H dpi:dpi panel:panel palette:dp];
    }
    if (g) {
        CGContextFlush(g);
        CGImageRef img = CGBitmapContextCreateImage(g);
        if (img) {
            CGContextRef cg = NSGraphicsContext.currentContext.CGContext;
            CGContextSaveGState(cg);
            CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
            CGContextDrawImage(cg, NSRectToCGRect(b), img);  // upright: a CGImage's first row is its top
            CGContextRestoreGState(cg);
            CGImageRelease(img);
        }
        CGContextRelease(g);
    }
    CGColorSpaceRelease(cs);
}

// The photoreal cells (Win32 drawPhotoCells + photoSpectrum/Signal/Bitrate/Frames): the classic
// painters' readings on a grid that scales with the meter. mac has no skin material, so the frame is
// Flat — the chrome band only, as Win32 draws for a skin without one.
- (void)drawPhotoCellsW:(int)W h:(int)H dpi:(UINT)dpi panel:(COLORREF)panel palette:(const MeterPalette&)dp {
    const int bz = photoBezelPx(H, dpi, SkinMaterial::Flat);
    const RECT in{bz, bz, W - bz, H - bz};
    if (in.right <= in.left || in.bottom <= in.top) return;
    const PhotoRows g = photoRows(in.bottom - in.top, dpi);
    _photoCells.clear();
    std::vector<PhotoCell>& out = _photoCells;
    const COLORREF low = cref(dp.low), mid = cref(dp.mid), high = cref(dp.high), accent = cref(dp.accent),
                   peak = cref(dp.peak), rawPeak = cref(_palette.peak);
    auto ramp = [&](float frac) { return frac < 0.60f ? low : frac < 0.85f ? mid : high; };
    const int Wd = in.right - in.left;
    switch (_kind) {
        case MeterKind::Spectrum: {
            const int bands = std::max(1, _drawBands);
            const PhotoCols cols = photoCols(Wd, std::max(2, Wd / bands), g.gap, bands);
            for (int c = 0; c < cols.cols; ++c) {  // mac's levels already carry the Sens gain
                const LONG x0 = in.left + cols.cellLeft(c), x1 = x0 + cols.colW - g.gap;
                const int litN = (int)std::lround(std::clamp(_specLevel[c], 0.0f, 1.0f) * g.rows);
                const int peakRow = (int)std::lround(std::clamp(_specPeak[c], 0.0f, 1.0f) * g.rows);
                for (int r = 0; r < g.rows; ++r) {
                    bool on = r < litN;
                    COLORREF lit = ramp((float)r / g.rows);
                    if (peakRow > 0 && r == peakRow) { on = true; lit = peak; }
                    out.push_back({RECT{x0, in.top + g.cellTop(r), x1, in.top + g.cellBottom(r)}, lit, on});
                }
            }
            break;
        }
        case MeterKind::Signal: {
            const int bars = 5;
            const PhotoCols cols = photoCols(Wd, std::max(2, Wd / bars), g.gap, bars);
            const float sl = std::clamp(_sigStrength * _gain, 0.0f, 1.0f);
            const int litBars = (int)std::ceil(sl * bars - 0.001f);
            COLORREF base = sl > 0.6f ? low : (sl > 0.3f ? mid : high);
            base = lerpCol(base, high, _sigTrouble * 0.6f);
            for (int j = 0; j < cols.cols; ++j) {
                const LONG x0 = in.left + cols.cellLeft(j), x1 = x0 + cols.colW - g.gap;
                const int n = photoSignalCells(j, g.rows);
                for (int r = 0; r < n; ++r)
                    out.push_back({RECT{x0, in.top + g.cellTop(r), x1, in.top + g.cellBottom(r)}, base, j < litBars});
            }
            break;
        }
        case MeterKind::Bitrate: {
            const PhotoCols cols = photoCols(Wd, photoBitrateColW(g), g.gap, kHist);
            const int n = std::min(cols.cols, kHist);
            for (int k = 0; k < n; ++k) {  // k = 0: the newest sample, in the rightmost column
                const int idx = ((_histHead - 1 - k) % kHist + kHist) % kHist;
                const float frac = std::clamp(_hist[idx] * _gain, 0.0f, 1.0f);
                const int litN = (int)std::lround(frac * g.rows);
                const LONG x0 = in.left + cols.cellLeft(cols.cols - 1 - k), x1 = x0 + cols.colW - g.gap;
                for (int r = 0; r < g.rows; ++r) {
                    // The ramp heats toward the RAW peak (meterDrawnPalette says why).
                    const COLORREF lit = lerpCol(accent, rawPeak, (float)r / g.rows * 0.5f);
                    out.push_back({RECT{x0, in.top + g.cellTop(r), x1, in.top + g.cellBottom(r)}, lit, r < litN});
                }
            }
            break;
        }
        case MeterKind::Frames: {
            const PhotoCols cols = photoCols(Wd, photoFramesColW(g), g.gap, 1 << 16);
            const float frac = std::clamp(_fps / 60.0f * _gain, 0.0f, 1.0f);
            const int lit = (int)std::lround(frac * cols.cols);
            const int fps = (int)std::lround(_fps);
            COLORREF base = fps >= 24 ? low : (fps >= 15 ? mid : high);
            base = lerpCol(base, high, _flare);
            for (int c = 0; c < cols.cols; ++c) {
                const LONG x0 = in.left + cols.cellLeft(c), x1 = x0 + cols.colW - g.gap;
                for (int r = 0; r < g.rows; ++r)
                    out.push_back({RECT{x0, in.top + g.cellTop(r), x1, in.top + g.cellBottom(r)}, base, c < lit});
            }
            break;
        }
    }
    PhotoScene sc;
    sc.finish = _style == MeterStyle::BacklitLcd ? CellFinish::Lcd : _style == MeterStyle::Vfd ? CellFinish::Vfd
                                                                                               : CellFinish::Led;
    sc.panel = panel;
    sc.off = cref(dp.off);
    sc.hot = rawPeak;  // an LED's core is "the hottest light" — the RAW peak
    sc.glow = _glow;
    sc.pitch = g.pitch;
    paintPhotoCells(_px.data(), W, H, in, sc, out, _photoCache);
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
