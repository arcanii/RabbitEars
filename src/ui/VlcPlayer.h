// SPDX-License-Identifier: GPL-3.0-or-later
// VlcPlayer — libVLC wrapper for embedded IPTV playback.
//
// All media-player lifecycle work runs on a dedicated worker thread, because
// libVLC 3.x's stop()/release() are SYNCHRONOUS and block until the stream
// actually tears down — which can hang for seconds on a stuck/dead stream. Doing
// that on the UI thread froze the app on channel switches. The UI thread only
// enqueues commands (play/stop/pause/volume) and returns immediately; the worker
// serializes them and owns the media player. libVLC events are marshaled to the
// UI thread via PostMessage.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

struct libvlc_instance_t;
struct libvlc_media_player_t;
struct libvlc_event_t;

namespace rabbitears {

enum class PlayerEvent : unsigned {
    Opening = 0,
    Buffering = 1,  // lParam = percent 0..100
    Playing = 2,
    Paused = 3,
    Stopped = 4,
    EndReached = 5,
    Error = 6,
};

class VlcPlayer {
public:
    VlcPlayer() = default;
    ~VlcPlayer();
    VlcPlayer(const VlcPlayer&) = delete;
    VlcPlayer& operator=(const VlcPlayer&) = delete;

    bool init();
    bool isReady() const { return inst_ != nullptr; }

    void setEventTarget(HWND target, UINT msg) { evtTarget_ = target; evtMsg_ = msg; }
    void attach(HWND video) { video_ = video; }

    // All of these return immediately; the worker performs the (possibly blocking)
    // libVLC work off the UI thread.
    bool play(const std::wstring& url, const std::wstring& userAgent = {},
              const std::wstring& referrer = {});
    void togglePause();
    void stop();
    void setVolume(int volume);            // 0..100
    int volume() const { return volume_.load(); }
    void setAspectRatio(const char* ar);   // "16:9" / "4:3" / nullptr
    bool isPlaying() const { return playing_.load(); }

    // Called from the libVLC event thread via a C thunk — do not call directly.
    void handleVlcEvent(const libvlc_event_t* e);

private:
    struct Cmd {
        enum Type { Play, Stop, Pause, Volume, Aspect, Quit } type;
        std::wstring url, userAgent, referrer;
        int          ivalue = 0;
        std::string  svalue;
    };
    void enqueue(Cmd c);
    void workerLoop();
    void doPlay(const Cmd& c);
    void doStop();
    bool hasNewerPlayOrStop();  // for coalescing rapid channel switches

    libvlc_instance_t*     inst_ = nullptr;
    libvlc_media_player_t* mp_ = nullptr;   // worker-thread only
    HWND                   video_ = nullptr;
    HWND                   evtTarget_ = nullptr;
    UINT                   evtMsg_ = 0;
    std::atomic<int>       volume_{80};
    std::atomic<bool>      playing_{false};

    std::thread              worker_;
    std::mutex               mtx_;
    std::condition_variable  cv_;
    std::deque<Cmd>          queue_;
    bool                     quit_ = false;
    bool                     started_ = false;
};

}  // namespace rabbitears
