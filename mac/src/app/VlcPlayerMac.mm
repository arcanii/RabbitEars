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

#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/Log.h"

#if defined(RABBITEARS_HAVE_LIBVLC)
#include <vlc/vlc.h>
#endif

namespace rabbitears {

struct VlcPlayerMac::Impl {
#if defined(RABBITEARS_HAVE_LIBVLC)
    libvlc_instance_t* vlc = nullptr;
    libvlc_media_player_t* player = nullptr;
    // Stats accumulators (main-thread only) for per-second deltas across samples.
    int  prevRead = 0, prevDemux = 0, prevCorrupted = 0, prevDiscont = 0, prevLost = 0, prevShown = 0;
    bool firstSample = true;
    std::chrono::steady_clock::time_point lastSample{};
#endif
    NSView* videoView = nil;  // weak; owned by the window
};

VlcPlayerMac::VlcPlayerMac() : impl_(new Impl) {
#if defined(RABBITEARS_HAVE_LIBVLC)
    // Point libVLC at its plugins unless VLC_PLUGIN_PATH is already set. Prefer the
    // plugins bundled in the app (Contents/PlugIns — a self-contained release);
    // fall back to the compile-time VLC.app path for non-bundled dev/CLI builds.
    if (!getenv("VLC_PLUGIN_PATH")) {
        NSString* bundled = NSBundle.mainBundle.builtInPlugInsPath;  // Contents/PlugIns
        BOOL isDir = NO;
        if (bundled.length &&
            [NSFileManager.defaultManager fileExistsAtPath:bundled isDirectory:&isDir] && isDir) {
            setenv("VLC_PLUGIN_PATH", bundled.fileSystemRepresentation, 1);
        }
#if defined(RABBITEARS_VLC_PLUGIN_PATH)
        else {
            setenv("VLC_PLUGIN_PATH", RABBITEARS_VLC_PLUGIN_PATH, 1);
        }
#endif
    }
    const char* args[] = {"--no-video-title-show"};
    impl_->vlc = libvlc_new(sizeof(args) / sizeof(args[0]), args);
    if (impl_->vlc) impl_->player = libvlc_media_player_new(impl_->vlc);
    diag::info(impl_->vlc ? L"libVLC instance created" : L"libVLC init FAILED (plugins/deps?)");
#endif
}

VlcPlayerMac::~VlcPlayerMac() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (impl_->player) {
        libvlc_media_player_stop(impl_->player);
        libvlc_media_player_release(impl_->player);
    }
    if (impl_->vlc) libvlc_release(impl_->vlc);
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

}  // namespace rabbitears
