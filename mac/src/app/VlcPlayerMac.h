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

    // Coarse playback state for the "Hide unavailable channels" heuristic: Playing = the stream is up;
    // Error = a FATAL player error (any — an open failure OR a mid-playback input error, and terminal);
    // Other = idle/opening/buffering/ended. The caller only demotes a channel to Dead on Error when it
    // never reached Playing this attempt (a true open failure), so a mid-playback blip can't latch it dead.
    enum class PlayState { Other, Playing, Error };
    PlayState playState() const;

    // Whether the current media has at least one audio track (false when stopped or for
    // a video-only stream). Gates the Spectrum consent cross-check so a legitimately
    // silent stream isn't mistaken for denied audio capture.
    bool hasAudioTrack() const;

    // ---- recording (independent headless second player) ----------------------------
    // Record the stream to `filePath` via a SECOND, headless libVLC media player that muxes
    // the elementary streams straight to disk (`:sout=#std{...}`, stream-copy, no re-encode,
    // no video output) — the mac peer of Win32 VlcPlayer::doRecordStart. It opens a SEPARATE
    // connection to `url`, independent of playback, so a pane can record while it plays (and
    // providers that cap concurrent connections per account may reject the second one).
    // `mux` is the libVLC container ("ts"/"mkv"/"mp4"). Returns false if the recorder can't
    // start. One recording per player; a second start stops the first.
    bool startRecording(const std::wstring& url, const std::wstring& userAgent,
                        const std::wstring& referrer, const std::wstring& filePath,
                        const std::string& mux);
    // Stop + finalize the recording (SYNCHRONOUS: flushes and, for mp4/mkv, writes the index
    // so the file is playable). Safe to call when not recording. Also run from the destructor,
    // so a pane torn down mid-recording finalizes its file on the teardown queue.
    void stopRecording();
    // Like stopRecording, but hands the (blocking) stop+release to a background GCD queue so a
    // stalled recording connection can't hang the UI — the mac peer of Win32's reaper thread.
    // Returns immediately; the file finalizes when the background stop completes. Use for the
    // user-facing Stop button; the destructor/quit paths use the synchronous stopRecording.
    void stopRecordingAsync();
    bool isRecording() const;
    std::wstring recordingFile() const;  // the path being written, or empty

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace rabbitears
