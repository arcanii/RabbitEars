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

    // Begin an ASYNCHRONOUS teardown (for a multi-view mode switch): hand the blocking
    // libVLC stop()/release() to a reaper thread and join only the now-free worker, so the
    // UI thread never blocks on a stuck stream. Poll teardownComplete(); once true, shutdown()
    // joins the finished reaper instantly. Idempotent; safe to still call shutdown() later.
    void beginTeardown();
    bool teardownComplete();  // true once every async-stop reaper has finished (UI-thread poll)
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

    // Vout-host pool (see the private VoutHost section for the why). The UI thread pre-creates
    // vout-host child windows inside each pane and registers them here; the worker attaches each
    // new stream to a proven-FREE one instead of reusing the pane HWND (which spawns libVLC's
    // "VLC (Direct3D11 output)" top-level window on rapid channel-surf).
    void registerVoutHost(HWND host);  // enqueues Cmd::AddHost (UI thread -> the worker owns hosts_)
    HWND currentHost() const { return currentHost_.load(); }  // host the live stream renders into
    void setVoutHostMsg(UINT msg) { voutHostMsg_ = msg; }  // WM_APP_MAKE_VOUT_HOST, for on-demand growth

    // All of these return immediately; the worker performs the (possibly blocking)
    // libVLC work off the UI thread.
    bool play(const std::wstring& url, const std::wstring& userAgent = {},
              const std::wstring& referrer = {});
    void togglePause();
    void stop();
    void setVolume(int volume);            // 0..100 (the active pane's level)
    int volume() const { return volume_.load(); }
    // Multi-view mute: true deselects this player's audio track (silent, and survives libVLC
    // recreating the audio output on a quality switch); false restores it. Only the active pane
    // is unmuted. Applied on the worker + re-asserted when the stream starts playing.
    void setMuted(bool muted);
    bool isMuted() const { return muted_.load(); }
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
        enum Type { Play, Stop, Pause, Volume, Aspect, Mute, Quit, RecordStart, RecordStop,
                    AddHost } type;
        std::wstring url, userAgent, referrer, recPath;
        int          ivalue = 0;
        std::string  svalue;
        void*        pvalue = nullptr;  // AddHost: the HWND of a newly-created vout host
    };
    void enqueue(Cmd c);
    void workerLoop();
    void doPlay(const Cmd& c);
    void doStop(bool async);  // async=true tears the old player down off-thread
    void reapAsyncStops();    // join+drop finished reaper threads (worker-thread only)
    HWND pickVoutHost();      // worker: a free vout host (reuse a drained one, else grow); never hangs
    void applyAudioState();   // worker: (un)select the audio track + volume per muted_ (see .cpp)
    void doRecordStart(const Cmd& c);      // worker-thread only
    void doRecordStop(bool async = false); // async=true offloads the recorder stop to a reaper
    void sampleStats();         // worker-thread only: read libVLC stats -> snapshot_
    bool hasNewerPlayOrStop();  // for coalescing rapid channel switches

    libvlc_instance_t*     inst_ = nullptr;
    libvlc_media_player_t* mp_ = nullptr;      // worker-thread only (playback)
    libvlc_media_player_t* rec_ = nullptr;     // worker-thread only (recorder)
    libvlc_media_t*        media_ = nullptr;   // worker-thread only (retained for stats)
    HWND                   video_ = nullptr;  // the parent pane HWND (never a set_hwnd target directly)
    HWND                   evtTarget_ = nullptr;
    UINT                   evtMsg_ = 0;
    int                    tag_ = 0;   // pane index echoed to the event target (HIWORD of wParam)

    // ---- vout-host pool (fixes the "VLC (Direct3D11 output)" popout on rapid channel-surf) ----
    // libVLC must NEVER set_hwnd a new media player onto a surface whose PREVIOUS vout hasn't been
    // released yet — it responds by spawning a top-level "VLC (Direct3D11 output)" window. So each
    // new stream attaches to a proven-FREE inner vout-host child window instead of reusing the pane
    // HWND: the old stream keeps ITS host until its reaper finishes releasing it, then that host
    // returns to the free set. Host WINDOWS are created/sized/shown/hidden on the UI thread (window
    // affinity); the pool + selection live here on the worker. hosts_ is worker-only, grown either
    // via Cmd::AddHost (pre-created hosts registered from addPane) or on-demand in pickVoutHost().
    struct VoutHost { HWND hwnd = nullptr; bool busy = false; unsigned long long busySeq = 0; };
    std::vector<VoutHost>  hosts_;                 // worker-thread only
    std::atomic<HWND>      currentHost_{nullptr};  // host the LIVE player renders into (worker writes, UI reads)
    unsigned long long     hostBusyCounter_ = 0;   // worker-only: monotonic tag for least-recently-busied
    UINT                   voutHostMsg_ = 0;       // WM_APP_MAKE_VOUT_HOST (UI creates a host on demand)
    std::atomic<int>       volume_{80};
    // Multi-view mute: a background (non-active) pane is silenced by DESELECTING its audio track
    // (libvlc_audio_set_track(mp, -1)), not by volume=0 — libVLC resets a player's volume to 100%
    // whenever it recreates the audio output (e.g. an HLS low->high quality switch, no event fired),
    // so a volume-based mute leaks/pulses on adaptive feeds. A pane with no audio track selected has
    // no audio output to reset. savedAudioTrack_ remembers the track to restore on unmute.
    std::atomic<bool>      muted_{false};
    int                    savedAudioTrack_ = -1;  // worker-only: track id to restore on unmute
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
    bool                     teardownStarted_ = false;  // beginTeardown() ran (idempotent guard)

    // Async teardown of superseded players: libVLC 3.x stop()/release() block until
    // a stream tears down (seconds on a stuck feed), which would wedge the worker and
    // the next channel switch. doStop(async) (playback) and doRecordStop(async) (recorder,
    // on a mode-switch teardown) offload them here; the worker prunes finished ones, and
    // the destructor drains all before libvlc_release(inst_).
    // `host` is the vout host the dying playback player was rendering into (nullptr for a recorder
    // reaper, which owns no host). reapAsyncStops() returns it to the free set once `done` flips.
    struct Reaper { std::thread th; std::shared_ptr<std::atomic<bool>> done; HWND host = nullptr; };
    std::vector<Reaper>      reapers_;  // worker-thread only (+ destructor after join)
};

}  // namespace rabbitears
