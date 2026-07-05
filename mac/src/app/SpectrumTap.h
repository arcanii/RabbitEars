// SPDX-License-Identifier: GPL-3.0-or-later
//
// SpectrumTap — a NON-INVASIVE audio spectrum source for SpectrumMeterView, the mac
// peer of Win32's WASAPI SpectrumTap. It installs a Core Audio *process tap*
// (macOS 14.2+) on THIS process's own audio output, runs an FFT over the tapped
// PCM, and reports normalized frequency-band magnitudes — all WITHOUT touching the
// playback path: libVLC keeps its own audio output, so there is no A/V desync (the
// reason the old libvlc_audio_set_callbacks tap was reverted). On macOS < 14.2 the
// initializer returns nil and the meter simply shows its "unavailable" placeholder;
// playback is unaffected either way.
//
// OPT-IN: creating the tap is what triggers the one-time OS audio-capture consent
// prompt, so it is only constructed on an explicit user action (View ▸ Show
// Spectrum). NOTE: on a DENIED prompt the tap still constructs successfully but
// delivers silence — the caller must detect that behaviourally (see the FlowStats
// cross-check in MainWindowController) rather than relying on a nil return.
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface SpectrumTap : NSObject

// Start observing this process's audio output. `handler` is called with `count`
// band magnitudes (each 0..1, low→high frequency) on a REAL-TIME audio thread —
// keep it trivial (SpectrumMeterView's -pushSpectrum:count: is a brief lock-guarded
// copy and is safe to call here). `bandCount` is clamped to [1, 64]. Returns nil on
// macOS < 14.2 or if the tap can't be created.
- (nullable instancetype)initWithBandCount:(int)bandCount
                           spectrumHandler:(void (^)(const float* bands, int count))handler;

- (void)stop;   // idempotent; also invoked from -dealloc

@end

NS_ASSUME_NONNULL_END
