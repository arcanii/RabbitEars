// SPDX-License-Identifier: GPL-3.0-or-later
//
// See VlcPlayerMac.h. The libVLC calls are compiled when RABBITEARS_HAVE_LIBVLC
// is defined (Mac.cmake sets it once libVLC is provisioned); otherwise the
// methods are safe no-ops that log, so the app still builds without a backend.
// The plugin path + runtime rpath are wired by mac/CMakeLists.txt.
//
// TODO(phase-1+): libVLC event -> dispatch_async(main) marshaling for state /
// buffering callbacks (the mac peer of VlcPlayer.cpp's PostMessageW path), and
// the buffer/spectrum meters.
#import "VlcPlayerMac.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "VlcEngineMac.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/Log.h"

#if defined(RABBITEARS_HAVE_LIBVLC)
#include <vlc/vlc.h>
#endif

namespace rabbitears {

struct VlcPlayerMac::Impl {
#if defined(RABBITEARS_HAVE_LIBVLC)
    libvlc_instance_t* vlc = nullptr;      // BORROWED from VlcEngineMac — never released here
    libvlc_media_player_t* player = nullptr;
    // Stats accumulators (main-thread only) for per-second deltas across samples.
    int  prevRead = 0, prevDemux = 0, prevCorrupted = 0, prevDiscont = 0, prevLost = 0, prevShown = 0;
    bool firstSample = true;
    std::chrono::steady_clock::time_point lastSample{};
    int  savedAudioTrack = -1;  // track id to restore on unmute (multi-view background panes)
#endif
    bool    muted = false;
    NSView* videoView = nil;  // weak; owned by the window
};

// The plugin-path setup + libvlc_new moved to VlcEngineMac (once per process); a player
// now just allocates state and creates its cheap media player in init(engine).
VlcPlayerMac::VlcPlayerMac() : impl_(new Impl) {}

bool VlcPlayerMac::init(VlcEngineMac& engine) {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->player) return true;                 // idempotent
    impl_->vlc = engine.handle();                   // borrowed; the engine owns it
    if (!impl_->vlc) return false;                  // engine not ready (libVLC absent)
    impl_->player = libvlc_media_player_new(impl_->vlc);
    if (impl_->player && impl_->videoView)          // attachTo may have run before init
        libvlc_media_player_set_nsobject(impl_->player, (__bridge void*)impl_->videoView);
    return impl_->player != nullptr;
#else
    (void)engine;
    diag::info(L"VlcPlayerMac::init (stub: libVLC not provisioned)");
    return false;
#endif
}

VlcPlayerMac::~VlcPlayerMac() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->player) {
        libvlc_media_player_stop(impl_->player);
        libvlc_media_player_release(impl_->player);
    }
    // impl_->vlc is owned by VlcEngineMac — do NOT release it here.
#endif
    delete impl_;
}

bool VlcPlayerMac::attachTo(NSView* videoView) {
    impl_->videoView = videoView;
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return false;
    // The macOS analogue of set_hwnd: hand libVLC the NSView to render into.
    libvlc_media_player_set_nsobject(impl_->player, (__bridge void*)videoView);
    return true;
#else
    diag::info(L"VlcPlayerMac::attachTo (stub: libVLC not provisioned)");
    return false;
#endif
}

void VlcPlayerMac::play(const std::wstring& url, const std::wstring& userAgent,
                        const std::wstring& referrer) {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->vlc || !impl_->player) return;
    libvlc_media_t* media = libvlc_media_new_location(impl_->vlc, utf8FromWide(url).c_str());
    if (!media) return;
    if (!userAgent.empty())
        libvlc_media_add_option(media, (":http-user-agent=" + utf8FromWide(userAgent)).c_str());
    if (!referrer.empty())
        libvlc_media_add_option(media, (":http-referrer=" + utf8FromWide(referrer)).c_str());
    libvlc_media_player_set_media(impl_->player, media);
    libvlc_media_release(media);
    libvlc_media_player_play(impl_->player);
    impl_->firstSample = true;  // fresh stats baseline for the new stream
#else
    (void)userAgent;
    (void)referrer;
    diag::info(L"VlcPlayerMac::play (stub): " + url);
#endif
}

void VlcPlayerMac::stop() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->player) libvlc_media_player_stop(impl_->player);
#endif
}

void VlcPlayerMac::setVolume(int percent) {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->player) libvlc_audio_set_volume(impl_->player, percent);
#else
    (void)percent;
#endif
}

// Silence a background pane by DESELECTING its audio track (survives libVLC recreating
// the audio output on a quality switch); unmute restores the saved track. See the header.
void VlcPlayerMac::setMuted(bool muted) {
    impl_->muted = muted;
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return;
    const int cur = libvlc_audio_get_track(impl_->player);  // -1 == nothing selected (silent)

    if (muted) {
        if (cur == -1) return;                             // already silent — no churn
        impl_->savedAudioTrack = cur;                      // remember the real track
        libvlc_audio_set_track(impl_->player, -1);         // -1 == "Disable" => silent
        return;
    }

    // ---- unmute ----
    // Idempotent: a track is already selected, so we are audible.
    if (cur != -1) return;
    // Nothing to select yet (media still opening, or a video-only stream). The caller
    // re-asserts this every stats tick, so we simply retry once the audio ES shows up.
    if (libvlc_audio_get_track_count(impl_->player) <= 0) return;

    // Select a track that ACTUALLY EXISTS right now. `savedAudioTrack` can go stale when the
    // stream re-opens its elementary streams (an ad break, an HLS quality switch) — selecting
    // a stale id fails silently and the pane stays mute forever. So prefer the saved id only
    // when it is still present, else fall back to the first real track.
    int restore = -1;
    libvlc_track_description_t* head = libvlc_audio_get_track_description(impl_->player);
    for (libvlc_track_description_t* t = head; t; t = t->p_next) {
        if (t->i_id == -1) continue;                       // the "Disable" pseudo-entry
        if (t->i_id == impl_->savedAudioTrack) { restore = t->i_id; break; }  // saved still valid
        if (restore == -1) restore = t->i_id;              // else remember the first real track
    }
    if (head) libvlc_track_description_list_release(head);
    if (restore != -1) {
        libvlc_audio_set_track(impl_->player, restore);
        impl_->savedAudioTrack = restore;
    }
#endif
}

bool VlcPlayerMac::isMuted() const { return impl_->muted; }

int VlcPlayerMac::audioTrack() const {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return -2;
    return libvlc_audio_get_track(impl_->player);
#else
    return -2;
#endif
}

// Peer of Win32 VlcPlayer::sampleStats. Reads libVLC's cumulative media stats and
// turns them into per-second rates (byte-counter deltas over wall-clock) + per-sample
// event deltas. Main-thread only (called from the UI's meter timer).
FlowStats VlcPlayerMac::sampleStats() {
    FlowStats fs;
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return fs;
    fs.playing = libvlc_media_player_is_playing(impl_->player) != 0;
    libvlc_media_t* media = libvlc_media_player_get_media(impl_->player);  // +1 ref
    if (!media) return fs;
    libvlc_media_stats_t s{};
    const bool ok = libvlc_media_get_stats(media, &s);
    libvlc_media_release(media);
    if (!ok) return fs;

    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - impl_->lastSample).count();
    if (dt < 0.001) dt = 0.001;

    // read - demux = data arrived off the network but not yet consumed ≈ buffered ahead.
    fs.bufferedBytes = std::max(0LL, (long long)s.i_read_bytes - (long long)s.i_demux_read_bytes);
    if (!impl_->firstSample) {
        // 32-bit cumulative counters can wrap on long streams; a negative delta = wrap → 0.
        const auto perSec = [dt](long long cur, long long prev) {
            const long long d = cur - prev;
            return d > 0 ? (double)d / dt : 0.0;
        };
        fs.demuxBytesPerSec   = perSec(s.i_demux_read_bytes, impl_->prevDemux);
        fs.readBytesPerSec    = perSec(s.i_read_bytes, impl_->prevRead);
        fs.corruptedDelta     = std::max(0, s.i_demux_corrupted - impl_->prevCorrupted);
        fs.discontinuityDelta = std::max(0, s.i_demux_discontinuity - impl_->prevDiscont);
        fs.lostPicturesDelta  = std::max(0, s.i_lost_pictures - impl_->prevLost);
        fs.displayedPerSec    = std::max(0, s.i_displayed_pictures - impl_->prevShown) / dt;
    }
    impl_->prevDemux     = s.i_demux_read_bytes;
    impl_->prevRead      = s.i_read_bytes;
    impl_->prevCorrupted = s.i_demux_corrupted;
    impl_->prevDiscont   = s.i_demux_discontinuity;
    impl_->prevLost      = s.i_lost_pictures;
    impl_->prevShown     = s.i_displayed_pictures;
    impl_->lastSample    = now;
    impl_->firstSample   = false;
#endif
    return fs;
}

bool VlcPlayerMac::hasAudioTrack() const {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return false;
    return libvlc_audio_get_track_count(impl_->player) > 0;
#else
    return false;
#endif
}

}  // namespace rabbitears
