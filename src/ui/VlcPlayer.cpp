// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VlcPlayer.h"

#include <vlc/vlc.h>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// C thunk registered with libVLC; forwards to the instance. Runs on a libVLC
// thread — only touches atomics + PostMessage (thread-safe), never mp_.
void vlcEventThunk(const libvlc_event_t* e, void* opaque) {
    static_cast<VlcPlayer*>(opaque)->handleVlcEvent(e);
}

}  // namespace

VlcPlayer::~VlcPlayer() {
    if (started_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit_ = true;
            queue_.push_back({Cmd::Quit});
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    if (inst_) {
        libvlc_release(inst_);
        inst_ = nullptr;
    }
}

bool VlcPlayer::init() {
    if (inst_) return true;
    const char* args[] = {
        "--intf=dummy",       "--no-video-title-show", "--no-osd", "--no-stats",
        "--network-caching=1000", "--http-reconnect",  "--quiet",
    };
    inst_ = libvlc_new(static_cast<int>(sizeof(args) / sizeof(args[0])), args);
    if (!inst_) return false;
    worker_ = std::thread(&VlcPlayer::workerLoop, this);
    started_ = true;
    return true;
}

void VlcPlayer::enqueue(Cmd c) {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(std::move(c));
    }
    cv_.notify_all();
}

bool VlcPlayer::hasNewerPlayOrStop() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (const Cmd& q : queue_)
        if (q.type == Cmd::Play || q.type == Cmd::Stop || q.type == Cmd::Quit) return true;
    return false;
}

void VlcPlayer::workerLoop() {
    for (;;) {
        Cmd c;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !queue_.empty(); });
            c = std::move(queue_.front());
            queue_.pop_front();
        }
        switch (c.type) {
            case Cmd::Quit:
                doStop();
                return;
            case Cmd::Play:
                // Coalesce rapid channel switches: if a newer Play/Stop is queued,
                // skip this one and let the latest win (avoids N blocking stops).
                if (hasNewerPlayOrStop()) break;
                doPlay(c);
                break;
            case Cmd::Stop:
                doStop();
                break;
            case Cmd::Pause:
                if (mp_) libvlc_media_player_pause(mp_);
                break;
            case Cmd::Volume:
                if (mp_) libvlc_audio_set_volume(mp_, c.ivalue);
                break;
            case Cmd::Aspect:
                if (mp_) libvlc_video_set_aspect_ratio(mp_, c.svalue.empty() ? nullptr : c.svalue.c_str());
                break;
        }
    }
}

void VlcPlayer::doStop() {
    if (mp_) {
        libvlc_media_player_stop(mp_);   // synchronous — but on the worker, not UI
        libvlc_media_player_release(mp_);
        mp_ = nullptr;
    }
    playing_.store(false);
}

void VlcPlayer::doPlay(const Cmd& c) {
    doStop();
    if (!inst_) return;
    const std::string u = utf8FromWide(c.url);
    libvlc_media_t* m = libvlc_media_new_location(inst_, u.c_str());
    if (!m) return;
    libvlc_media_add_option(m, ":network-caching=1500");
    if (!c.userAgent.empty())
        libvlc_media_add_option(m, (":http-user-agent=" + utf8FromWide(c.userAgent)).c_str());
    if (!c.referrer.empty())
        libvlc_media_add_option(m, (":http-referrer=" + utf8FromWide(c.referrer)).c_str());

    mp_ = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (!mp_) return;

    if (video_) libvlc_media_player_set_hwnd(mp_, static_cast<void*>(video_));
    if (auto* em = libvlc_media_player_event_manager(mp_)) {
        for (int ev : {libvlc_MediaPlayerOpening, libvlc_MediaPlayerBuffering,
                       libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused,
                       libvlc_MediaPlayerStopped, libvlc_MediaPlayerEndReached,
                       libvlc_MediaPlayerEncounteredError})
            libvlc_event_attach(em, ev, vlcEventThunk, this);
    }
    libvlc_audio_set_volume(mp_, volume_.load());
    libvlc_media_player_play(mp_);
}

void VlcPlayer::handleVlcEvent(const libvlc_event_t* e) {
    PlayerEvent ev = PlayerEvent::Opening;
    LPARAM lp = 0;
    switch (e->type) {
        case libvlc_MediaPlayerOpening: ev = PlayerEvent::Opening; break;
        case libvlc_MediaPlayerBuffering:
            ev = PlayerEvent::Buffering;
            lp = static_cast<LPARAM>(e->u.media_player_buffering.new_cache);
            break;
        case libvlc_MediaPlayerPlaying: ev = PlayerEvent::Playing; playing_.store(true); break;
        case libvlc_MediaPlayerPaused: ev = PlayerEvent::Paused; break;
        case libvlc_MediaPlayerStopped: ev = PlayerEvent::Stopped; playing_.store(false); break;
        case libvlc_MediaPlayerEndReached: ev = PlayerEvent::EndReached; playing_.store(false); break;
        case libvlc_MediaPlayerEncounteredError: ev = PlayerEvent::Error; playing_.store(false); break;
        default: return;
    }
    if (evtTarget_ && evtMsg_)
        PostMessageW(evtTarget_, evtMsg_, static_cast<WPARAM>(ev), lp);
}

bool VlcPlayer::play(const std::wstring& url, const std::wstring& userAgent,
                     const std::wstring& referrer) {
    if (!inst_) return false;
    Cmd c{Cmd::Play};
    c.url = url;
    c.userAgent = userAgent;
    c.referrer = referrer;
    enqueue(std::move(c));
    return true;
}

void VlcPlayer::togglePause() { enqueue({Cmd::Pause}); }
void VlcPlayer::stop() { enqueue({Cmd::Stop}); }

void VlcPlayer::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_.store(volume);
    Cmd c{Cmd::Volume};
    c.ivalue = volume;
    enqueue(std::move(c));
}

void VlcPlayer::setAspectRatio(const char* ar) {
    Cmd c{Cmd::Aspect};
    if (ar) c.svalue = ar;
    enqueue(std::move(c));
}

}  // namespace rabbitears
