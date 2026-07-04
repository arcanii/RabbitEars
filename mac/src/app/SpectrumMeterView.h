// SPDX-License-Identifier: GPL-3.0-or-later
//
// SpectrumMeterView — an LED spectrum-analyser strip fed by SpectrumTap's FFT bands
// (opt-in, audio-capture). Vertical LED columns per band, low->high frequency left
// to right, coloured from the shared meter palette (rabbitears::mac::MeterPalette).
//
// It is the SEPARATE, opt-in strip that sits below the always-on libVLC-stats meter
// (StatMeterView). Because audio capture can be DENIED (the tap still "succeeds" but
// delivers silence — the reason the meter was pulled before), this view has an
// explicit -setAvailable: state: NO renders a "grant permission" placeholder, so it
// is NEVER a dark, undetectable strip. The owning controller decides available vs
// not by cross-checking FlowStats (audio flowing but tap silent => denied).
#pragma once

#import <Cocoa/Cocoa.h>

#include "MeterModel.h"  // rabbitears::mac::MeterPalette

@interface SpectrumMeterView : NSView
// Push a new spectrum frame (`count` values 0..1, low->high). THREAD-SAFE — called
// from SpectrumTap's real-time audio thread; latched under a brief lock and eased on
// the view's ~30fps timer.
- (void)pushSpectrum:(const float*)bands count:(int)count;
// Recolour from the shared meter model (defaults to the classic palette at init).
- (void)setPalette:(const rabbitears::mac::MeterPalette&)palette;
// Live spectrum (YES) vs the "grant audio permission" placeholder (NO).
- (void)setAvailable:(BOOL)available;
// Whether any band exceeded a small energy floor since the last call (THREAD-SAFE;
// clears on read). The controller polls this to tell live audio from a silent/denied
// tap — the FlowStats cross-check that drives -setAvailable:.
- (BOOL)consumeHadEnergy;
- (void)reset;  // clear bands to idle (e.g. on stop)
@end
