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

    // Whether this player is holding (or about to hold) a connection to the provider — true from
    // play() until libVLC reaches a terminal state. The peer of Win32 VlcPlayer::isEngaged(), and
    // the gate the Xtream VOD sync consults: on a max_connections:1 line a sync that runs during
    // playback does not queue, it KICKS the user's stream.
    //
    // Why it is DERIVED from the polled state rather than a flag set in play(): this class attaches
    // no libVLC event callbacks at all, so a stored flag would have no reliable clear site, and a
    // geo-blocked channel (libvlc_Error) or a film watched to the end (libvlc_Ended) would latch it
    // true forever — refusing every future sync while nothing is playing. Win32 tried exactly that
    // (the pane's nowPlayingId) and rejected it by name. playState() alone is not enough either: it
    // collapses Opening/Buffering into `Other`, and that window is SECONDS long on an IPTV line
    // while the socket is already claimed. Hence the short grace window after play(), which
    // EXPIRES — that is the whole difference from a latch. Main thread only.
    bool isEngaged() const;

    // Whether the current media has at least one audio track (false when stopped or for
    // a video-only stream). Gates the Spectrum consent cross-check so a legitimately
    // silent stream isn't mistaken for denied audio capture.
    bool hasAudioTrack() const;

    // ---- position / seek / pause (the VOD transport) --------------------------------
    //
    // LIVE getters: each asks libVLC on the spot. No cached state, no atomics, no worker —
    // unlike Win32's VlcPlayer, whose WORKER owns `mp_` and must therefore publish position
    // through relaxed atomics. Here the caller IS the thread that owns the player, so a cache
    // would only add staleness. It also means a stopped player needs no explicit reset:
    // libVLC reports -1/-1/false the moment the input is gone, which is exactly what Win32's
    // doStop() zeroing exists to fake (and its videoWH_, which is never reset, is stale).
    //
    // MAIN THREAD ONLY, like playState()/sampleStats(). Cost was MEASURED against the vendored
    // libVLC 3.0.23, not assumed: get_time ~0.05 µs, get_length ~0.06 µs, is_seekable ~0.16 µs
    // — together a rounding error at the 250 ms tick, and less than the audio-track enumeration
    // setMuted() already does there on EVERY pane. They also do NOT block on a wedged feed (the
    // input thread parks in read()/connect() without holding the player lock).
    //
    // ⚠ THE ONE INVARIANT THIS DEPENDS ON: these can block for the full duration of a concurrent
    // libvlc_media_player_stop(), because they take the lock stop() holds. That is safe today
    // only because -teardownPane: re-points the active-pane aliases and pops the pane BEFORE
    // handing the player to its background stop, and play()'s internal stop() runs on this same
    // thread. If either ever moves off-main, this tick becomes blockable — start here.
    long long timeMs() const;    // 0 when there is no input (libVLC's -1 is normalized)
    long long lengthMs() const;  // 0 for a live stream — which is what keeps the bar hidden

    // Whether libVLC says this media can actually be repositioned. ASK LIBVLC — never infer
    // seekability from a URL suffix, a group title or Channel::Kind: catch-up and timeshift
    // feeds are live URLs that ARE seekable, an HLS DVR window on an ordinary live channel is
    // legitimately seekable with a short length, and VOD behind a dead link is not seekable at
    // all. A stream may also report a length while refusing to seek.
    bool isSeekable() const;

    // Jump to `ms`. Re-checks seekability against the LIVE player and clamps into range, so a
    // stale UI cannot ask a live stream to seek. Deliberately does NOT re-sample afterwards:
    // libVLC often reports the new time before the demuxer has actually repositioned, so an
    // immediate re-read would clear the UI's post-seek latch early and reproduce the thumb
    // snap-back the latch exists to hide. Seeking a network stream re-buffers — call it on drag
    // RELEASE, not per drag tick.
    //
    // RETURNS the position actually requested of libVLC after clamping, or -1 if it refused (no
    // player, or the media is not seekable). The caller needs this: the UI latches the seek
    // target to stop the thumb snapping back, and latching the UNCLAMPED request made a skip
    // near the end display a position past the media's own duration ("1:45:05 / 1:45:00"). The
    // clamp rule stays here, in one place, instead of being duplicated by every caller.
    long long seekTo(long long ms);

    // Pause/resume. libVLC's set_pause is a no-op on a stream that cannot pause, so this is
    // safe to call on live TV; isPaused() reports libVLC's own state rather than a local flag,
    // so it cannot drift from reality.
    void setPaused(bool paused);
    bool isPaused() const;

    // The decoded video's pixel size; false / 0×0 until the vout is up. Live-polled like the
    // rest, so it cannot go stale after a stop. Consumer: the PiP inset aspect snap.
    bool videoSize(unsigned& w, unsigned& h) const;

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

// ---- detached players still shutting down (the mac peer of Win32's `dyingPanes`) --------------
//
// libVLC's stop()/release() BLOCK for seconds on a stuck IPTV feed, so mac tears a removed pane
// down on a background queue (-teardownPane:) and stops a recorder the same way
// (stopRecordingAsync). Both hand the player off and return immediately — after which the object
// is unreachable, isEngaged()/isRecording() cannot be asked, and yet its SOCKET is still open.
//
// Win32 keeps those players in a `dyingPanes` vector and walks it in the sync gate. mac has no
// such collection, so this counter is the equivalent: a detached player is in flight while it is
// non-zero. Anything that must not contend for the provider connection has to consult it, or the
// gate has a hole exactly as wide as a slow teardown.
//
// Thread-safe (atomic); Begin/End must be paired around the whole background teardown.
int  vlcDetachedPlayerCount();
void vlcDetachedPlayerBegin();
void vlcDetachedPlayerEnd();

}  // namespace rabbitears
