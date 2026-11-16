// SPDX-License-Identifier: GPL-3.0-or-later
// See TankView.h.
#import "TankView.h"
#import "Tr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "TankSim.h"
#include "ui/GlassMask.h"
#import "MeterView.h"  // REMeterGlass — ONE strength, shared with the meters

using namespace rabbitears;
using namespace rabbitears::mac;
using namespace rabbitears::i18n;

namespace {
std::atomic<uint32_t> g_fluid{kDefaultFluidColor};
std::atomic<bool> g_scaledDots{false};
}  // namespace

void REDataFlowSetFluidColor(unsigned r, unsigned g, unsigned b) { g_fluid = RGB(r, g, b); }
void REDataFlowFluidColor(unsigned* r, unsigned* g, unsigned* b) {
    const COLORREF c = g_fluid.load();
    if (r) *r = GetRValue(c);
    if (g) *g = GetGValue(c);
    if (b) *b = GetBValue(c);
}
void REDataFlowSetScaledDots(bool on) { g_scaledDots = on; }
bool REDataFlowScaledDots() { return g_scaledDots.load(); }

@implementation TankView {
    TankSim                _sim;
    std::vector<uint32_t>  _px;
    NSTimer*               _timer;     // ~30 fps while the tank has something to show
    NSString*              _metrics;
    __weak TankView*       _mirror;
    std::vector<uint8_t>   _glassAdd, _glassMul;
    float                  _glassBuilt;
    int                    _glassW, _glassH, _glassChrome;
}

- (instancetype)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) {
        self.wantsLayer = YES;
        NSMenu* m = [[NSMenu alloc] init];
        NSMenuItem* hide = [m addItemWithTitle:Tr(StringId::BufferMeterHide) action:@selector(hideClicked:) keyEquivalent:@""];
        hide.target = self;
        self.menu = m;
    }
    return self;
}

- (void)dealloc { [_timer invalidate]; }
- (BOOL)isFlipped { return NO; }

- (void)hideClicked:(id)__unused sender {
    if (self.onHideRequested) self.onHideRequested();
}

- (void)relabelForLanguageChange {
    self.menu.itemArray.firstObject.title = Tr(StringId::BufferMeterHide);
}

// Run the ~30 fps sim only while it has something to do (Win32 startTimer / the WM_TIMER stop test).
- (void)syncTimer {
    if (_sim.active() && !_timer && !self.hidden) {
        __weak TankView* weak = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:0.033 repeats:YES block:^(NSTimer* t) {
            TankView* s = weak;
            if (!s) { [t invalidate]; return; }
            s->_sim.step();
            [s setNeedsDisplay:YES];
            if (!s->_sim.active()) { [t invalidate]; s->_timer = nil; }
        }];
        _timer.tolerance = 0.005;
    }
}

- (void)setHidden:(BOOL)hidden {
    [super setHidden:hidden];
    if (hidden) { [_timer invalidate]; _timer = nil; }
    else [self syncTimer];
}

- (void)setHealth:(int)percent {
    [_mirror setHealth:percent];
    if (percent < 0) return;  // paused: keep the level
    _sim.setHealth(percent);
    if (percent <= 0) _metrics = nil;  // no stream: no readout
    [self syncTimer];
}

- (void)setFlow:(float)flowRate trouble:(float)trouble {
    [_mirror setFlow:flowRate trouble:trouble];
    _sim.setFlow(flowRate, trouble);
    [self syncTimer];
}

- (void)setMetrics:(NSString*)text {
    [_mirror setMetrics:text];
    _metrics = text.length ? [text copy] : nil;
    [self setNeedsDisplay:YES];
}

- (void)setMirror:(TankView*)mirror {
    if (mirror == self) mirror = nil;
    _mirror = mirror;
    if (!mirror) return;
    mirror->_sim.seedFrom(_sim);  // start the twin where this tank IS
    mirror->_metrics = _metrics;
    [mirror syncTimer];
    [mirror setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)__unused dirty {
    const NSRect b = self.bounds;
    const NSRect pb = [self convertRectToBacking:b];
    const int W = (int)std::lround(pb.size.width), H = (int)std::lround(pb.size.height);
    if (W < 4 || H < 4) return;
    const float scale = (float)(pb.size.width / std::max<CGFloat>(1, b.size.width));
    _px.assign((size_t)W * (size_t)H, 0);
    TankColors c;
    c.fluid = g_fluid.load();
    c.scaledDots = g_scaledDots.load();
    // The glass (Win32 ensureGlass/applyGlass): the grid insets by exactly the band the mask will use.
    const UINT dpi = (UINT)std::lround(96.0f * scale);
    const float glass = REMeterGlass();
    const int chrome = MulDiv(2, (int)dpi, 96);
    int inset = 0;
    if (glass > 0.0f) {
        if (_glassBuilt != glass || _glassW != W || _glassH != H || _glassChrome != chrome) {
            buildGlassMask(W, H, GlassParams{glass, chrome}, _glassAdd, _glassMul);
            _glassBuilt = glass;
            _glassW = W;
            _glassH = H;
            _glassChrome = chrome;
        }
        inset = glassChromePx(W, H, chrome);
    }
    _sim.render(_px.data(), W, H, dpi, c, inset);
    if (glass > 0.0f && _glassAdd.size() == _px.size() && _glassMul.size() == _px.size()) {
        auto* p = reinterpret_cast<uint8_t*>(_px.data());
        for (size_t i = 0; i < _px.size(); ++i) {
            const unsigned mul = _glassMul[i], add = _glassAdd[i];
            for (int ch = 0; ch < 3; ++ch) {
                unsigned v = p[i * 4 + ch];
                v = (v * mul) / 255u + add;
                p[i * 4 + ch] = (uint8_t)(v > 255u ? 255u : v);
            }
        }
    }
    const CGFloat insetPt = inset / std::max<CGFloat>(1, scale);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef g = CGBitmapContextCreate(_px.data(), (size_t)W, (size_t)H, 8, (size_t)W * 4, cs,
                                           (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (g) {
        CGImageRef img = CGBitmapContextCreateImage(g);
        if (img) {
            CGContextRef cg = NSGraphicsContext.currentContext.CGContext;
            CGContextSaveGState(cg);
            CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
            CGContextDrawImage(cg, NSRectToCGRect(b), img);
            CGContextRestoreGState(cg);
            CGImageRelease(img);
        }
        CGContextRelease(g);
    }
    CGColorSpaceRelease(cs);
    if (_metrics.length) {
        // Sized from the tank's height, 9..11 pt (Win32 drawMetrics: 0.3 of the height), top-right,
        // light text with a black drop shadow (the mac surround is dark).
        const CGFloat pt = std::clamp<CGFloat>(std::round(b.size.height * 0.3 * 30.0 / 24.0), 9, 11);
        NSShadow* sh = [[NSShadow alloc] init];
        sh.shadowColor = NSColor.blackColor;
        sh.shadowOffset = NSMakeSize(1, -1);
        sh.shadowBlurRadius = 0;
        NSDictionary* a = @{NSFontAttributeName: [NSFont systemFontOfSize:pt weight:NSFontWeightSemibold],
                            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.92 alpha:1.0],
                            NSShadowAttributeName: sh};
        const NSSize sz = [_metrics sizeWithAttributes:a];
        // Inside the glass bezel when there is one (Win32 drawMetrics' inset rule).
        const CGFloat right = std::max<CGFloat>(4, insetPt + 2);
        [_metrics drawAtPoint:NSMakePoint(b.size.width - sz.width - right, b.size.height - sz.height - 1 - insetPt)
               withAttributes:a];
    }
}

@end
