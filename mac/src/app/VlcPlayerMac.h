// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS libVLC player wrapper. This is a FROM-SCRATCH reimplementation of the
// Win32 src/ui/VlcPlayer.{h,cpp}, NOT a port: the Windows version binds video to
// an HWND (libvlc_media_player_set_hwnd) and marshals libVLC events back to the
// UI thread with PostMessageW. The macOS version binds to an NSView
// (libvlc_media_player_set_nsobject) and marshals events via dispatch_async to
// the main queue. Only the libVLC call sequence is shared knowledge.
#pragma once

#import <Cocoa/Cocoa.h>

#include <string>

namespace rabbitears {

// A snapshot of real stream health, computed from libVLC media stats (the mac peer
// of Win32's FlowStats in Win32/ui/VlcPlayer.h — mac-local for now; a later phase
// promotes it to common/ for the shared meter model). Byte rates are per-second
// deltas over wall-clock; the *Delta fields count events since the previous sample.
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

class VlcPlayerMac {
public:
    VlcPlayerMac();
    ~VlcPlayerMac();

    VlcPlayerMac(const VlcPlayerMac&) = delete;
    VlcPlayerMac& operator=(const VlcPlayerMac&) = delete;

    // Bind the player's video output to a layer-backed NSView.
    bool attachTo(NSView* videoView);

    // Start playback of a stream, applying the M3U per-channel VLC options.
    void play(const std::wstring& url, const std::wstring& userAgent, const std::wstring& referrer);
    void stop();
    void setVolume(int percent);  // 0..100

    // Sample libVLC's media stats into a FlowStats snapshot (per-second byte rates
    // over wall-clock + per-sample event deltas). Call on a steady ~250ms timer from
    // the main thread — the deltas are stateful. Drives the buffer/bitrate/signal/
    // frames meters with NO audio capture (no consent prompt, no A/V desync).
    FlowStats sampleStats();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rabbitears
