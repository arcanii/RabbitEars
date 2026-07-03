// SPDX-License-Identifier: GPL-3.0-or-later
//
// See VlcPlayerMac.h. SCAFFOLD STATE: the libVLC calls are compiled only when
// RABBITEARS_HAVE_LIBVLC is defined (set by mac/cmake/Mac.cmake once libVLC is
// provisioned for macOS). Until then the methods are safe no-ops that log, so
// the app builds and the window opens without a media backend.
//
// TODO(phase-1): provision libVLC for macOS (VLCKit or the libvlc SDK dylibs +
// headers), fill in the event-attach + NSNotification/dispatch marshaling that
// replaces VlcPlayer.cpp's PostMessageW path, and wire the buffer/spectrum
// meters to the mac UI.
#import "VlcPlayerMac.h"

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
    const char* args[] = {"--no-video-title-show"};
    impl_->vlc = libvlc_new(sizeof(args) / sizeof(args[0]), args);
    if (impl_->vlc) impl_->player = libvlc_media_player_new(impl_->vlc);
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
