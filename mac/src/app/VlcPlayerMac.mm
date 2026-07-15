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
    libvlc_media_player_t* recorder = nullptr;  // headless second player muxing to a file
    // Stats accumulators (main-thread only) for per-second deltas across samples.
    int  prevRead = 0, prevDemux = 0, prevCorrupted = 0, prevDiscont = 0, prevLost = 0, prevShown = 0;
    bool firstSample = true;
    std::chrono::steady_clock::time_point lastSample{};
    int  savedAudioTrack = -1;  // track id to restore on unmute (multi-view background panes)
#endif
    bool         muted = false;
    std::wstring recFile;     // path the recorder is writing, or empty
    NSView*      videoView = nil;  // weak; owned by the window
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
    stopRecording();  // finalize + release the recorder BEFORE the player / the borrowed instance
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
    // Tear down any current input BEFORE swapping media. set_media on a still-running player is
    // unreliable when switching between live streams — the old input can wedge (last frame
    // frozen) while the new one never starts, especially for a background PiP inset re-play
    // (reported on some IPTV feeds; test VOD streams switch cleanly either way). A stop first
    // makes the switch deterministic. Idempotent no-op on an idle player (first play).
    libvlc_media_player_stop(impl_->player);
    impl_->savedAudioTrack = -1;  // the new media has its own tracks; drop the stale restore id
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

// Coarse state for the "Hide unavailable channels" dead-status heuristic (see VlcPlayerMac.h).
VlcPlayerMac::PlayState VlcPlayerMac::playState() const {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return PlayState::Other;
    switch (libvlc_media_player_get_state(impl_->player)) {
        case libvlc_Playing: return PlayState::Playing;
        case libvlc_Error:   return PlayState::Error;
        default:             return PlayState::Other;
    }
#else
    return PlayState::Other;
#endif
}

bool VlcPlayerMac::hasAudioTrack() const {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return false;
    // NOT libvlc_audio_get_track_count(): that counts the "-1 Disable" pseudo-entry too, so it
    // can report a track on a stream that carries no audio ES at all. The only caller gates the
    // Spectrum "grant permission" placeholder, and a false positive there nags the user about a
    // permission that is fine — on a video-only stream nothing would ever feed the tap, the
    // cumulative silent-tick counter would run to its threshold, and the placeholder would
    // appear. Walk the descriptions and count tracks that actually exist.
    libvlc_track_description_t* head = libvlc_audio_get_track_description(impl_->player);
    bool real = false;
    for (libvlc_track_description_t* t = head; t && !real; t = t->p_next)
        real = (t->i_id != -1);
    if (head) libvlc_track_description_list_release(head);
    return real;
#else
    return false;
#endif
}

// ---- recording (headless second player) -----------------------------------------------

bool VlcPlayerMac::startRecording(const std::wstring& url, const std::wstring& userAgent,
                                  const std::wstring& referrer, const std::wstring& filePath,
                                  const std::string& mux) {
#if defined(RABBITEARS_HAVE_LIBVLC)
    stopRecording();  // one recording per player
    if (!impl_->vlc) return false;
    libvlc_media_t* m = libvlc_media_new_location(impl_->vlc, utf8FromWide(url).c_str());
    if (!m) { diag::error(L"record: media_new failed for " + url); return false; }

    // Stream-copy to a file: no re-encode, low CPU. Single-quote the dst so spaces survive,
    // and DOUBLE any literal ' — VLC's config-chain parser ends a single-quoted value at the
    // first ' unless doubled (the ~/Movies path can hold one, e.g. a user named O'Brien; the
    // channel-name component is already scrubbed of ' by the caller). Paths are native '/'.
    std::string dst = utf8FromWide(filePath);
    for (size_t i = 0; (i = dst.find('\'', i)) != std::string::npos; i += 2) dst.insert(i, 1, '\'');
    const std::string container = mux.empty() ? "ts" : mux;
    libvlc_media_add_option(m, (":sout=#std{access=file,mux=" + container + ",dst='" + dst + "'}").c_str());
    libvlc_media_add_option(m, ":sout-keep");
    if (!userAgent.empty())
        libvlc_media_add_option(m, (":http-user-agent=" + utf8FromWide(userAgent)).c_str());
    if (!referrer.empty())
        libvlc_media_add_option(m, (":http-referrer=" + utf8FromWide(referrer)).c_str());

    impl_->recorder = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (!impl_->recorder) { diag::error(L"record: player_new failed"); return false; }
    if (libvlc_media_player_play(impl_->recorder) != 0) {  // headless (no set_nsobject) -> muxed to file
        diag::error(L"record: play failed to start");
        libvlc_media_player_release(impl_->recorder);
        impl_->recorder = nullptr;
        return false;
    }
    impl_->recFile = filePath;
    diag::info(L"recording started -> " + filePath);
    return true;
#else
    (void)url; (void)userAgent; (void)referrer; (void)filePath; (void)mux;
    return false;
#endif
}

void VlcPlayerMac::stopRecording() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->recorder) {
        libvlc_media_player_stop(impl_->recorder);     // flush + finalize (mp4/mkv write their index here)
        libvlc_media_player_release(impl_->recorder);
        impl_->recorder = nullptr;
        diag::info(L"recording stopped");
    }
#endif
    impl_->recFile.clear();
}

void VlcPlayerMac::stopRecordingAsync() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    libvlc_media_player_t* dying = impl_->recorder;
    impl_->recorder = nullptr;      // detach now so isRecording() is immediately false
    impl_->recFile.clear();
    if (!dying) return;
    // Hand the blocking stop()+release() to a background queue (peer of Win32's reaper thread):
    // a stalled recorder connection or a slow mp4 index write can't hang the UI. The borrowed
    // libVLC instance is an app-lifetime object (it leaks at quit, never released), so this
    // detached stop can safely run even past termination — no use-after-free of the instance.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        libvlc_media_player_stop(dying);
        libvlc_media_player_release(dying);
        diag::info(L"recording stopped (async)");
    });
#endif
}

bool VlcPlayerMac::isRecording() const {
#if defined(RABBITEARS_HAVE_LIBVLC)
    return impl_->recorder != nullptr;
#else
    return false;
#endif
}

std::wstring VlcPlayerMac::recordingFile() const { return impl_->recFile; }

}  // namespace rabbitears
