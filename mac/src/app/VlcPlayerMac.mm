// SPDX-License-Identifier: GPL-3.0-or-later
// See VlcPlayerMac.h. The libVLC calls are compiled when RABBITEARS_HAVE_LIBVLC
// is defined (Mac.cmake sets it once libVLC is provisioned); otherwise the
// methods are safe no-ops that log, so the app still builds without a backend.
// The plugin path + runtime rpath are wired by mac/CMakeLists.txt.
//
// Audio tap (the mac start of the Win32 SpectrumTap): when a level callback is
// set, libVLC's decoded PCM is intercepted (libvlc_audio_set_callbacks), the peak
// is metered, and the samples are re-output through an AudioQueue so playback is
// unchanged. Set the callback before play() to take effect.
#import "VlcPlayerMac.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/Log.h"

#if defined(RABBITEARS_HAVE_LIBVLC)
#import <AudioToolbox/AudioToolbox.h>
#include <vlc/vlc.h>
#endif

namespace rabbitears {

struct VlcPlayerMac::Impl {
#if defined(RABBITEARS_HAVE_LIBVLC)
    libvlc_instance_t* vlc = nullptr;
    libvlc_media_player_t* player = nullptr;
    AudioQueueRef aq = nullptr;              // re-output for the tap (created on first tapped play)

    // libVLC audio callback (its audio thread) + AudioQueue recycle callback.
    // Static members so they can touch Impl's private-to-VlcPlayerMac layout.
    static void audioPlay(void* data, const void* samples, unsigned count, int64_t pts);
    static void aqDone(void* user, AudioQueueRef aq, AudioQueueBufferRef buf);
    void ensureQueue();
#endif
    NSView* videoView = nil;                 // weak; owned by the window
    std::function<void(float)> levelCb;      // set => audio tap is enabled
};

#if defined(RABBITEARS_HAVE_LIBVLC)

void VlcPlayerMac::Impl::audioPlay(void* data, const void* samples, unsigned count, int64_t /*pts*/) {
    auto* im = static_cast<Impl*>(data);
    const int16_t* s = static_cast<const int16_t*>(samples);
    const unsigned n = count * 2;  // interleaved stereo (S16N, 2ch)
    int32_t peak = 0;
    for (unsigned i = 0; i < n; ++i) {
        const int32_t v = std::abs(static_cast<int>(s[i]));
        if (v > peak) peak = v;
    }
    if (im->levelCb) im->levelCb(static_cast<float>(peak) / 32768.0f);

    if (im->aq) {
        const UInt32 bytes = count * 4;  // 2ch * 2 bytes
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(im->aq, bytes, &buf) == noErr) {
            std::memcpy(buf->mAudioData, samples, bytes);
            buf->mAudioDataByteSize = bytes;
            AudioQueueEnqueueBuffer(im->aq, buf, 0, nullptr);
        }
    }
}

void VlcPlayerMac::Impl::aqDone(void* /*user*/, AudioQueueRef aq, AudioQueueBufferRef buf) {
    AudioQueueFreeBuffer(aq, buf);  // we allocate per-callback
}

void VlcPlayerMac::Impl::ensureQueue() {
    if (aq) return;
    AudioStreamBasicDescription asbd = {};
    asbd.mSampleRate = 48000;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel = 16;
    asbd.mChannelsPerFrame = 2;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 4;
    asbd.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&asbd, aqDone, this, nullptr, nullptr, 0, &aq) == noErr && aq) {
        AudioQueueStart(aq, nullptr);
    }
}

#endif  // RABBITEARS_HAVE_LIBVLC

VlcPlayerMac::VlcPlayerMac() : impl_(new Impl) {
#if defined(RABBITEARS_HAVE_LIBVLC)
#if defined(RABBITEARS_VLC_PLUGIN_PATH)
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
    if (impl_->aq) {
        AudioQueueStop(impl_->aq, true);
        AudioQueueDispose(impl_->aq, true);
    }
    if (impl_->vlc) libvlc_release(impl_->vlc);
#endif
    delete impl_;
}

void VlcPlayerMac::setLevelCallback(std::function<void(float)> cb) { impl_->levelCb = std::move(cb); }

bool VlcPlayerMac::attachTo(NSView* videoView) {
    impl_->videoView = videoView;
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (!impl_->player) return false;
    libvlc_media_player_set_nsobject(impl_->player, (__bridge void*)videoView);  // mac analogue of set_hwnd
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

    // Enable the metering tap before the media plays (set_callbacks must precede play).
    if (impl_->levelCb) {
        impl_->ensureQueue();
        libvlc_audio_set_format(impl_->player, "S16N", 48000, 2);
        libvlc_audio_set_callbacks(impl_->player, &Impl::audioPlay, nullptr, nullptr, nullptr, nullptr,
                                   impl_);
    }

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
    if (impl_->aq) AudioQueueReset(impl_->aq);
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
