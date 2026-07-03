// SPDX-License-Identifier: GPL-3.0-or-later
// See VlcPlayerMac.h. The libVLC calls are compiled when RABBITEARS_HAVE_LIBVLC
// is defined (Mac.cmake sets it once libVLC is provisioned); otherwise the
// methods are safe no-ops that log, so the app still builds without a backend.
// The plugin path + runtime rpath are wired by mac/CMakeLists.txt.
//
// Audio tap (the mac start of the Win32 SpectrumTap): when a level callback is
// set, libVLC's decoded PCM is intercepted (libvlc_audio_set_callbacks), the peak
// is metered, and the samples are re-output through an AudioQueue (fed from a
// fixed, recycled buffer pool) so playback is unchanged.
#import "VlcPlayerMac.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

    // AudioQueue re-output for the tap, fed from a fixed pool of buffers recycled
    // between the audio thread (audioPlay) and the AudioQueue thread (aqDone) — no
    // realtime allocation, bounded memory, no leak on a failed/stalled enqueue.
    AudioQueueRef aq = nullptr;
    static constexpr int kNumBufs = 8;
    static constexpr UInt32 kMaxBufBytes = 128 * 1024;  // ~680ms @ 48k/stereo/s16 (chunks are far smaller)
    std::mutex poolMx;
    std::vector<AudioQueueBufferRef> freeBufs;  // ready-to-fill buffers

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

    if (!im->aq) return;
    const UInt32 bytes = count * 4;  // 2ch * 2 bytes
    AudioQueueBufferRef buf = nullptr;
    {
        std::lock_guard<std::mutex> lk(im->poolMx);
        if (!im->freeBufs.empty()) { buf = im->freeBufs.back(); im->freeBufs.pop_back(); }
    }
    if (!buf) return;  // pool exhausted (queue stalled) — drop this chunk; the meter already updated
    if (bytes > buf->mAudioDataBytesCapacity) {
        std::lock_guard<std::mutex> lk(im->poolMx);
        im->freeBufs.push_back(buf);  // oversized (shouldn't happen) — recycle + drop
        return;
    }
    std::memcpy(buf->mAudioData, samples, bytes);
    buf->mAudioDataByteSize = bytes;
    if (AudioQueueEnqueueBuffer(im->aq, buf, 0, nullptr) != noErr) {
        std::lock_guard<std::mutex> lk(im->poolMx);
        im->freeBufs.push_back(buf);  // enqueue failed (e.g. mid-reset) — recycle, don't leak
    }
}

void VlcPlayerMac::Impl::aqDone(void* user, AudioQueueRef /*aq*/, AudioQueueBufferRef buf) {
    auto* im = static_cast<Impl*>(user);
    std::lock_guard<std::mutex> lk(im->poolMx);
    im->freeBufs.push_back(buf);  // back to the pool (allocated once in ensureQueue)
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
        for (int i = 0; i < kNumBufs; ++i) {
            AudioQueueBufferRef b = nullptr;
            if (AudioQueueAllocateBuffer(aq, kMaxBufBytes, &b) == noErr) freeBufs.push_back(b);
        }
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
        libvlc_media_player_stop(impl_->player);   // quiesces audioPlay
        libvlc_media_player_release(impl_->player);
    }
    if (impl_->aq) {
        AudioQueueStop(impl_->aq, true);            // synchronous — quiesces aqDone
        AudioQueueDispose(impl_->aq, true);         // frees the pool buffers
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

    // Tear down the previous stream before re-arming the tap: set_format/callbacks
    // want a stopped player, and stop() AudioQueueResets so stale PCM from the old
    // channel doesn't bleed through. (First play() is a no-op stop.)
    stop();

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
    if (impl_->aq) AudioQueueReset(impl_->aq);  // fires aqDone for enqueued buffers (recycles them)
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
