// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterView — one Cocoa view that renders any meter KIND in any STYLE from a
// MeterConfig, the mac peer of the Win32 MiniMeter control. Kinds:
//   * Spectrum — FFT bands (fed from SpectrumTap; the only audio-capture one).
//   * Signal   — 5 antenna-style strength bars (composite stream health).
//   * Bitrate  — a scrolling throughput history.
//   * Frames   — displayed fps as lit cells, with a red flare on dropped frames.
// Styles: LED (dot-matrix), LCD (ghosted matrix), Tube (glowing matrix), and Scope
// (a phosphor trace of the level series). Colours + tuning come from the MeterConfig.
//
// The Spectrum kind folds in what SpectrumMeterView had: a thread-safe push from the
// real-time audio thread, an energy probe, and a "grant permission" placeholder so a
// denied tap is never a dark strip. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

#include "MeterModel.h"

@interface MeterView : NSView
- (instancetype)initWithKind:(rabbitears::mac::MeterKind)kind;
- (rabbitears::mac::MeterKind)kind;

// Look: style + palette (+ tuning). Defaults to the classic LED palette at init.
- (void)setConfig:(const rabbitears::mac::MeterConfig&)config;

- (void)reset;  // clear animated state to idle (e.g. on stop)

// ---- data feeds (push the one that matches -kind) ---------------------------
- (void)pushSpectrum:(const float*)bands count:(int)count;  // Spectrum — RT audio thread
- (void)setSignal:(float)strength trouble:(float)trouble;   // Signal   — UI thread (0..1 each)
- (void)pushBitrate:(double)bytesPerSec;                    // Bitrate  — UI thread
- (void)setFrames:(int)fps dropsDelta:(int)dropsDelta;      // Frames   — UI thread

// ---- Spectrum consent helpers (no-ops for other kinds) ----------------------
- (void)setAvailable:(BOOL)available;  // NO => "grant audio permission" placeholder
- (BOOL)consumeHadEnergy;              // any band > floor since last call (thread-safe, clears)
@end
