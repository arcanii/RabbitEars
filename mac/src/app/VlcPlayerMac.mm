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
#endif
    NSView* videoView = nil;  // weak; owned by the window
};

VlcPlayerMac::VlcPlayerMac() : impl_(new Impl) {
#if defined(RABBITEARS_HAVE_LIBVLC)
#if defined(RABBITEARS_VLC_PLUGIN_PATH)
    // Point libVLC at the provisioned plugins tree (unless the environment already
    // set one). Needed when loading a relocated libvlc.dylib whose compiled-in
    // plugin path doesn't match where the plugins actually live.
    if (!getenv("VLC_PLUGIN_PATH")) setenv("VLC_PLUGIN_PATH", RABBITEARS_VLC_PLUGIN_PATH, 1);
#endif
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

}  // namespace rabbitears
