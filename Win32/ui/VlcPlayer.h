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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "models/FlowStats.h"

struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;
struct libvlc_event_t;

namespace rabbitears {

class VlcEngine;  // owns the shared libVLC instance (VlcEngine.h); players borrow its handle

enum class PlayerEvent : unsigned {
    Opening = 0,
    Buffering = 1,  // lParam = percent 0..100
    Playing = 2,
    Paused = 3,
    Stopped = 4,
    EndReached = 5,
    Error = 6,
    Stats = 7,      // periodic throughput/packet-health sample; read via flowStats()
};

class VlcPlayer {
public:
    VlcPlayer() = default;
    ~VlcPlayer();
    // Synchronously tear down: join the worker + all reaper threads. Idempotent. The
    // shared libVLC instance is owned by VlcEngine and released separately (shut every
    // player down first). Called from WM_DESTROY so the worker/reaper threads are joined
    // before the message loop exits — a lingering process would block the auto-update installer.
    void shutdown();
    VlcPlayer(const VlcPlayer&) = delete;
    VlcPlayer& operator=(const VlcPlayer&) = delete;

    // Bind this player to the shared engine's libVLC instance and start its worker
    // thread. Returns false if the engine isn't initialized. Idempotent.
    bool init(VlcEngine& engine);
    bool isReady() const { return inst_ != nullptr; }

    void setEventTarget(HWND target, UINT msg) { evtTarget_ = target; evtMsg_ = msg; }
    // A small integer echoed in the HIWORD of every posted event's wParam (the LOWORD is
    // the PlayerEvent), so a multi-pane host can tell which player fired it. Default 0.
    void setTag(int tag) { tag_ = tag; }
    void attach(HWND video) { video_ = video; }

    // All of these return immediately; the worker performs the (possibly blocking)
    // libVLC work off the UI thread.
    bool play(const std::wstring& url, const std::wstring& userAgent = {},
              const std::wstring& referrer = {});
    void togglePause();
    void stop();
    void setVolume(int volume);            // 0..100
    int volume() const { return volume_.load(); }
    // Network buffer depth in ms (libVLC network-caching) — the receive->show
    // latency. Applied to the next stream opened (re-play to apply immediately).
    void setNetworkCaching(int ms);
    int networkCaching() const { return cachingMs_.load(); }
    void setAspectRatio(const char* ar);   // "16:9" / "4:3" / nullptr
    bool isPlaying() const { return playing_.load(); }

    // Latest stream-health snapshot (thread-safe copy). Refreshed each time a
    // PlayerEvent::Stats is posted while a stream is loaded.
    FlowStats flowStats() const;

    // Recording — a headless second player records `url` to `filePath` (a .ts
    // stream copy, no re-encode) on the shared instance, independent of playback,
    // so you can keep watching (even another channel). Returns immediately.
    bool startRecording(const std::wstring& url, const std::wstring& userAgent,
                        const std::wstring& referrer, const std::wstring& filePath,
                        const std::string& mux = "ts");  // "ts" | "mkv" (stream copy)
    void stopRecording();
    bool isRecording() const { return recording_.load(); }
    std::wstring recordingFile() const;  // thread-safe copy of the current path

    // Called from the libVLC event thread via a C thunk — do not call directly.
    void handleVlcEvent(const libvlc_event_t* e);

private:
    struct Cmd {
        enum Type { Play, Stop, Pause, Volume, Aspect, Quit, RecordStart, RecordStop } type;
        std::wstring url, userAgent, referrer, recPath;
        int          ivalue = 0;
        std::string  svalue;
    };
    void enqueue(Cmd c);
    void workerLoop();
    void doPlay(const Cmd& c);
    void doStop(bool async);  // async=true tears the old player down off-thread
    void reapAsyncStops();    // join+drop finished reaper threads (worker-thread only)
    void doRecordStart(const Cmd& c);  // worker-thread only
    void doRecordStop();               // worker-thread only
    void sampleStats();         // worker-thread only: read libVLC stats -> snapshot_
    bool hasNewerPlayOrStop();  // for coalescing rapid channel switches

    libvlc_instance_t*     inst_ = nullptr;
    libvlc_media_player_t* mp_ = nullptr;      // worker-thread only (playback)
    libvlc_media_player_t* rec_ = nullptr;     // worker-thread only (recorder)
    libvlc_media_t*        media_ = nullptr;   // worker-thread only (retained for stats)
    HWND                   video_ = nullptr;
    HWND                   evtTarget_ = nullptr;
    UINT                   evtMsg_ = 0;
    int                    tag_ = 0;   // pane index echoed to the event target (HIWORD of wParam)
    std::atomic<int>       volume_{80};
    std::atomic<int>       cachingMs_{1500};  // network-caching applied at media open
    std::atomic<bool>      playing_{false};
    std::atomic<bool>      recording_{false};
    mutable std::mutex     recMtx_;   // guards recFile_
    std::wstring           recFile_;  // current recording path (empty when idle)

    // Stats accumulators — touched only on the worker thread.
    int  prevReadBytes_ = 0, prevDemuxBytes_ = 0;
    int  prevCorrupted_ = 0, prevDiscontinuity_ = 0, prevLostPictures_ = 0;
    int  prevDisplayedPictures_ = 0;
    bool firstSample_ = true;
    std::chrono::steady_clock::time_point lastSampleTime_{};
    mutable std::mutex statsMtx_;   // guards snapshot_ (worker writes, UI reads)
    FlowStats           snapshot_;

    std::thread              worker_;
    std::mutex               mtx_;
    std::condition_variable  cv_;
    std::deque<Cmd>          queue_;
    bool                     quit_ = false;
    bool                     started_ = false;
    bool                     shutDown_ = false;  // shutdown() ran (idempotent guard)

    // Async teardown of superseded players: libVLC 3.x stop()/release() block until
    // a stream tears down (seconds on a stuck feed), which would wedge the worker and
    // the next channel switch. doStop(async) offloads them here; the worker prunes
    // finished ones, and the destructor drains all before libvlc_release(inst_).
    struct Reaper { std::thread th; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Reaper>      reapers_;  // worker-thread only (+ destructor after join)
};

}  // namespace rabbitears
