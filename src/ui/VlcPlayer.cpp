// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VlcPlayer.h"

#include <vlc/vlc.h>

#include "platform/Encoding.h"

namespace rabbitears {

VlcPlayer::~VlcPlayer() {
    destroyPlayer();
    if (inst_) {
        libvlc_release(inst_);
        inst_ = nullptr;
    }
}

bool VlcPlayer::init() {
    if (inst_) return true;
    // Robust arg set for an embedded IPTV player: no interface/OSD chrome, sane
    // network caching, auto-reconnect, quiet logging.
    const char* args[] = {
        "--intf=dummy",
        "--no-video-title-show",
        "--no-osd",
        "--no-stats",
        "--network-caching=1000",
        "--http-reconnect",
        "--quiet",
    };
    inst_ = libvlc_new(static_cast<int>(sizeof(args) / sizeof(args[0])), args);
    return inst_ != nullptr;
}

void VlcPlayer::destroyPlayer() {
    if (mp_) {
        libvlc_media_player_stop(mp_);
        libvlc_media_player_release(mp_);
        mp_ = nullptr;
    }
}

// The libVLC callback: posts a PlayerEvent to the UI target. Declared as a
// plain function with C linkage-compatible signature.
static void vlcEventThunk(const libvlc_event_t* e, void* opaque);

// Small per-player context so the C callback can reach the target HWND.
namespace {
struct PostCtx {
    HWND target;
    UINT msg;
};
}  // namespace

bool VlcPlayer::play(const std::wstring& url, const std::wstring& userAgent,
                     const std::wstring& referrer) {
    if (!inst_) return false;
    destroyPlayer();

    const std::string u = utf8FromWide(url);
    libvlc_media_t* m = libvlc_media_new_location(inst_, u.c_str());
    if (!m) return false;

    libvlc_media_add_option(m, ":network-caching=1500");
    if (!userAgent.empty())
        libvlc_media_add_option(m, (":http-user-agent=" + utf8FromWide(userAgent)).c_str());
    if (!referrer.empty())
        libvlc_media_add_option(m, (":http-referrer=" + utf8FromWide(referrer)).c_str());

    mp_ = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (!mp_) return false;

    // Render into our child window — MUST precede play().
    if (video_) libvlc_media_player_set_hwnd(mp_, static_cast<void*>(video_));

    // Wire events -> UI thread. The opaque points at a heap PostCtx owned for the
    // player's lifetime (freed in destroyPlayer via the event manager teardown).
    if (evtTarget_ && evtMsg_) {
        if (auto* em = libvlc_media_player_event_manager(mp_)) {
            static PostCtx ctx;  // single UI target for the app's one player
            ctx.target = evtTarget_;
            ctx.msg = evtMsg_;
            libvlc_event_attach(em, libvlc_MediaPlayerOpening, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerBuffering, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerPlaying, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerPaused, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerStopped, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerEndReached, vlcEventThunk, &ctx);
            libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, vlcEventThunk, &ctx);
        }
    }

    libvlc_audio_set_volume(mp_, volume_);
    return libvlc_media_player_play(mp_) == 0;
}

void VlcPlayer::togglePause() {
    if (mp_) libvlc_media_player_pause(mp_);  // libVLC toggles play/pause
}

void VlcPlayer::stop() { destroyPlayer(); }

void VlcPlayer::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_ = volume;
    if (mp_) libvlc_audio_set_volume(mp_, volume_);
}

void VlcPlayer::setAspectRatio(const char* ar) {
    if (mp_) libvlc_video_set_aspect_ratio(mp_, ar);
}

bool VlcPlayer::isPlaying() const { return mp_ && libvlc_media_player_is_playing(mp_); }

// ---------------------------------------------------------------------------

static void vlcEventThunk(const libvlc_event_t* e, void* opaque) {
    auto* ctx = static_cast<PostCtx*>(opaque);
    if (!ctx || !ctx->target) return;
    PlayerEvent ev = PlayerEvent::Opening;
    LPARAM lp = 0;
    switch (e->type) {
        case libvlc_MediaPlayerOpening: ev = PlayerEvent::Opening; break;
        case libvlc_MediaPlayerBuffering:
            ev = PlayerEvent::Buffering;
            lp = static_cast<LPARAM>(e->u.media_player_buffering.new_cache);
            break;
        case libvlc_MediaPlayerPlaying: ev = PlayerEvent::Playing; break;
        case libvlc_MediaPlayerPaused: ev = PlayerEvent::Paused; break;
        case libvlc_MediaPlayerStopped: ev = PlayerEvent::Stopped; break;
        case libvlc_MediaPlayerEndReached: ev = PlayerEvent::EndReached; break;
        case libvlc_MediaPlayerEncounteredError: ev = PlayerEvent::Error; break;
        default: return;
    }
    // Thread-safe: PostMessage marshals to the UI thread.
    PostMessageW(ctx->target, ctx->msg, static_cast<WPARAM>(ev), lp);
}

}  // namespace rabbitears
