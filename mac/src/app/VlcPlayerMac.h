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

#include "models/FlowStats.h"

namespace rabbitears {

class VlcEngineMac;  // owns the shared libVLC instance (VlcEngineMac.h); players borrow its handle

class VlcPlayerMac {
public:
    VlcPlayerMac();
    ~VlcPlayerMac();

    VlcPlayerMac(const VlcPlayerMac&) = delete;
    VlcPlayerMac& operator=(const VlcPlayerMac&) = delete;

    // Create this player's media player on the shared engine's libVLC instance. Must be
    // called once before play/attachTo. Returns false if the engine isn't ready (libVLC
    // absent). Idempotent. Multiple players share one engine (Split/2×2/PiP panes).
    bool init(VlcEngineMac& engine);

    // Bind the player's video output to a layer-backed NSView.
    bool attachTo(NSView* videoView);

    // Start playback of a stream, applying the M3U per-channel VLC options.
    void play(const std::wstring& url, const std::wstring& userAgent, const std::wstring& referrer);
    void stop();
    void setVolume(int percent);  // 0..100

    // Multi-view mute: a background (non-active) pane is silenced by DESELECTING its audio
    // track (libvlc_audio_set_track(mp, -1)), not by volume=0 — libVLC resets a player's
    // volume to 100% whenever it recreates the audio output (e.g. an HLS quality switch,
    // no event fired), so a volume-based mute leaks on adaptive feeds. Only the active pane
    // is unmuted. Idempotent; re-assert after (re)starting a stream.
    void setMuted(bool muted);
    bool isMuted() const;
    // The audio track id libVLC currently has selected: -1 == none (deselected/silent),
    // -2 == no player. Ground truth for the multi-view single-audio model.
    int audioTrack() const;

    // Sample libVLC's media stats into a FlowStats snapshot (per-second byte rates
    // over wall-clock + per-sample event deltas). Call on a steady ~250ms timer from
    // the main thread — the deltas are stateful. Drives the buffer/bitrate/signal/
    // frames meters with NO audio capture (no consent prompt, no A/V desync).
    FlowStats sampleStats();

    // Whether the current media has at least one audio track (false when stopped or for
    // a video-only stream). Gates the Spectrum consent cross-check so a legitimately
    // silent stream isn't mistaken for denied audio capture.
    bool hasAudioTrack() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rabbitears
