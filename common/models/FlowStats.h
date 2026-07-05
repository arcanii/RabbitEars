// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlowStats — a platform-neutral snapshot of real stream health, computed from
// libVLC's media stats (libvlc_media_get_stats). Byte rates are per-second deltas
// measured over wall-clock (unambiguous B/s); the *Delta fields count events since
// the previous sample, so a sampler must call at a steady cadence and keep its own
// running counters.
//
// This is the shared source of truth for both players — Win32 VlcPlayer
// (worker-thread sampleStats, consumed by the buffer/mini meters) and macOS
// VlcPlayerMac (main-thread sampleStats, consumed by StatMeterView). It drives the
// stats meters with NO audio capture, so no consent prompt and no A/V desync.
// Pure POD: no platform types, compiles into RabbitEarsCore on both platforms.
#pragma once

namespace rabbitears {

struct FlowStats {
    double    demuxBytesPerSec   = 0.0;  // data the demux consumed (playback throughput)
    double    readBytesPerSec    = 0.0;  // bytes read off the network (arrival rate)
    double    displayedPerSec    = 0.0;  // video frames displayed/sec (≈ effective fps)
    int       corruptedDelta     = 0;    // demux-corrupted blocks since last sample
    int       discontinuityDelta = 0;    // demux discontinuities since last sample
    int       lostPicturesDelta  = 0;    // dropped video frames since last sample
    long long bufferedBytes      = 0;    // read minus demux = data buffered ahead
    bool      playing            = false;
};

}  // namespace rabbitears
