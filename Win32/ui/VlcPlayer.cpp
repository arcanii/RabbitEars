// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VlcPlayer.h"

#include <algorithm>
#include <climits>  // ULLONG_MAX (least-recently-busied host fallback)

#include <vlc/vlc.h>

#include "ui/VlcEngine.h"
#include "platform/Encoding.h"
#include "platform/Log.h"

namespace rabbitears {
namespace {

// While a stream is loaded the worker wakes on this cadence to sample libVLC's
// media stats (throughput + packet health) even when no command is queued.
constexpr int kStatsPollMs = 250;

// C thunk registered with libVLC; forwards to the instance. Runs on a libVLC
// thread — only touches atomics, PostMessage, and the command queue (all
// thread-safe); never mp_ directly (that's worker-thread-owned).
void vlcEventThunk(const libvlc_event_t* e, void* opaque) {
    static_cast<VlcPlayer*>(opaque)->handleVlcEvent(e);
}

}  // namespace

void VlcPlayer::shutdown() {
    if (shutDown_) return;  // idempotent: WM_DESTROY calls this, then ~VlcPlayer again
    shutDown_ = true;
    if (started_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit_ = true;
            queue_.push_back({Cmd::Quit});
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    // Drain any in-flight async stops before releasing the instance they ran on
    // (worker is already joined, so reapers_ is ours now).
    for (auto& r : reapers_)
        if (r.th.joinable()) r.th.join();
    reapers_.clear();
    // The libVLC instance is owned by VlcEngine — just drop our borrowed handle so no
    // further libVLC work runs; the engine releases the instance once every player
    // built on it has been shut down.
    inst_ = nullptr;
}

void VlcPlayer::beginTeardown() {
    if (shutDown_ || teardownStarted_) return;
    teardownStarted_ = true;
    if (started_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back({Cmd::Stop});  // doStop(async): the blocking playback stop/release runs on a reaper
            Cmd rec{Cmd::RecordStop};
            rec.ivalue = 1;  // async: a recording pane's stop/release ALSO runs on a reaper, so the
            queue_.push_back(std::move(rec));  // worker join below can't block the UI on a stuck recorder feed
            quit_ = true;
            queue_.push_back({Cmd::Quit});  // then quit the worker (its own doStop/doRecordStop are no-ops now)
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();  // FAST: both stops were handed to reapers, not the worker
    }
    // reapers_ is ours now (worker joined). Do NOT join it here — the reaper runs the blocking
    // stop in the background so the UI thread stays responsive; teardownComplete() polls its done
    // flag and shutdown() (called once complete) joins the finished thread instantly.
}

bool VlcPlayer::teardownComplete() {
    for (auto& r : reapers_)
        if (!r.done->load()) return false;
    return true;
}

VlcPlayer::~VlcPlayer() { shutdown(); }

bool VlcPlayer::init(VlcEngine& engine) {
    if (inst_) return true;
    // Borrow the shared libVLC instance — VlcEngine owns it (one libvlc_new per
    // process; that single instance backs every player). All we spin up here is this
    // player's own worker thread.
    inst_ = engine.handle();
    if (!inst_) {
        diag::error(L"VlcPlayer::init called with an uninitialized VlcEngine");
        return false;
    }
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
        bool haveCmd = false;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            // With a stream loaded, wake periodically to sample stats; otherwise
            // sleep until a command arrives. (mp_ is worker-thread-owned, so
            // reading it here on the worker thread is race-free.)
            if (mp_)
                cv_.wait_for(lk, std::chrono::milliseconds(kStatsPollMs),
                             [&] { return !queue_.empty(); });
            else
                cv_.wait(lk, [&] { return !queue_.empty(); });
            if (!queue_.empty()) {
                c = std::move(queue_.front());
                queue_.pop_front();
                haveCmd = true;
            }
        }
        if (!haveCmd) {
            // Backstop the multi-view mute each poll: if an adaptive quality switch re-selected a
            // muted pane's audio track (recreating its output), deselect it again; keep the active
            // pane's volume applied. Guarded by get_track so we only touch it on an actual change
            // (no repeated set_track glitch). Cheap; keeps only the active pane audible.
            if (mp_) {
                if (muted_.load()) {
                    const int cur = libvlc_audio_get_track(mp_);
                    if (cur != -1) {  // a quality switch re-selected the track -> remember + redeselect
                        savedAudioTrack_ = cur;
                        libvlc_audio_set_track(mp_, -1);
                    }
                } else {
                    libvlc_audio_set_volume(mp_, volume_.load());
                }
            }
            sampleStats();  // periodic wake while playing
            continue;
        }
        switch (c.type) {
            case Cmd::Quit:
                doStop(/*async=*/false);  // synchronous so shutdown is clean
                doRecordStop();  // finalize any recording before shutdown
                return;
            case Cmd::Play:
                // Coalesce rapid channel switches: if a newer Play/Stop is queued,
                // skip this one and let the latest win (avoids N blocking stops).
                if (hasNewerPlayOrStop()) break;
                doPlay(c);
                break;
            case Cmd::Stop:
                doStop(/*async=*/true);
                break;
            case Cmd::Pause:
                if (mp_) libvlc_media_player_pause(mp_);
                break;
            case Cmd::Volume:
                if (mp_) libvlc_audio_set_volume(mp_, c.ivalue);
                break;
            case Cmd::Mute:
                applyAudioState();
                break;
            case Cmd::Aspect:
                if (mp_) libvlc_video_set_aspect_ratio(mp_, c.svalue.empty() ? nullptr : c.svalue.c_str());
                break;
            case Cmd::RecordStart:
                doRecordStart(c);
                break;
            case Cmd::RecordStop:
                doRecordStop(/*async=*/c.ivalue != 0);  // async on a teardown; sync on a manual stop
                break;
            case Cmd::AddHost:
                // A vout host the UI thread pre-created inside this player's pane. hosts_ is
                // worker-only, so it joins the pool here (never on the UI thread) — no race.
                if (c.pvalue)
                    hosts_.push_back({static_cast<HWND>(c.pvalue), false, 0});
                break;
        }
    }
}

void VlcPlayer::reapAsyncStops() {
    for (auto it = reapers_.begin(); it != reapers_.end();) {
        if (it->done->load()) {
            if (it->th.joinable()) it->th.join();
            const HWND freed = it->host;  // the dying player's vout host is now released
            it = reapers_.erase(it);
            if (freed)  // return it to the free set (a recorder reaper has none)
                for (auto& h : hosts_)
                    if (h.hwnd == freed) { h.busy = false; break; }
        } else {
            ++it;
        }
    }
}

// Worker-thread only. Pick a vout host for a new stream: reuse a free one (a just-drained host
// whose reaper has finished is legitimately free), else ask the UI thread to grow the pool, else
// (shutdown edge only) fall back to the least-recently-busied host. Never blocks forever — the
// growth request uses SendMessageTimeout(SMTO_ABORTIFHUNG) so shutdown()'s worker_.join() can't
// deadlock against a UI thread that's parked in that join.
HWND VlcPlayer::pickVoutHost() {
    for (auto& h : hosts_)
        if (!h.busy && IsWindow(h.hwnd)) return h.hwnd;  // a proven-free host
    // None free: ask the UI thread to create one (window affinity). Bounded 1s timeout: in normal
    // operation the UI is pumping and returns fast; only on the shutdown edge does it time out.
    if (voutHostMsg_ && evtTarget_ && IsWindow(evtTarget_)) {
        DWORD_PTR res = 0;
        const LRESULT ok = SendMessageTimeoutW(evtTarget_, voutHostMsg_,
                                               reinterpret_cast<WPARAM>(video_),
                                               static_cast<LPARAM>(tag_), SMTO_ABORTIFHUNG, 1000,
                                               &res);
        if (ok && res) {
            HWND nh = reinterpret_cast<HWND>(res);
            hosts_.push_back({nh, false, 0});  // worker owns hosts_ — safe to add here
            return nh;
        }
    }
    // Fallback (timeout/failure): reuse the host that was busied longest ago (its reaper is the
    // most likely to have finished). Never hang. Only reachable while the UI is in worker_.join().
    HWND best = nullptr;
    unsigned long long oldest = ULLONG_MAX;
    for (auto& h : hosts_)
        if (IsWindow(h.hwnd) && h.busySeq <= oldest) { oldest = h.busySeq; best = h.hwnd; }
    return best;  // nullptr only if the pool is somehow empty (doPlay then falls back to video_)
}

void VlcPlayer::doStop(bool async) {
    if (mp_) {
        if (async) {
            // Hand the (blocking) stop()/release() to a reaper thread so the worker
            // stays free to open the next channel immediately — a stuck/dead stream's
            // stop() can otherwise hang the worker for seconds and leave the next
            // channel un-connected until the app is restarted.
            libvlc_media_player_t* old = mp_;
            libvlc_media_t* oldMedia = media_;
            mp_ = nullptr;
            media_ = nullptr;
            // The vout host this dying player renders into stays BUSY until its reaper finishes
            // releasing it (its D3D vout still owns that surface); only then is it reusable. The
            // next stream must therefore pick a DIFFERENT free host — reusing this one now is
            // exactly the popout bug. currentHost_ goes null so the pick can't land back on it.
            const HWND oldHost = currentHost_.load();
            for (auto& h : hosts_)
                if (h.hwnd == oldHost) { h.busy = true; h.busySeq = ++hostBusyCounter_; break; }
            currentHost_.store(nullptr);
            // Detach our callbacks so this dying player can't post stale events (e.g.
            // a Stopped/Error for the channel we just switched away from).
            if (auto* em = libvlc_media_player_event_manager(old))
                for (int ev : {libvlc_MediaPlayerOpening, libvlc_MediaPlayerBuffering,
                               libvlc_MediaPlayerPlaying, libvlc_MediaPlayerPaused,
                               libvlc_MediaPlayerStopped, libvlc_MediaPlayerEndReached,
                               libvlc_MediaPlayerEncounteredError})
                    libvlc_event_detach(em, ev, vlcEventThunk, this);
            reapAsyncStops();  // drop any that have already finished
            auto done = std::make_shared<std::atomic<bool>>(false);
            reapers_.push_back({std::thread([old, oldMedia, done] {
                                    libvlc_media_player_stop(old);
                                    libvlc_media_player_release(old);
                                    if (oldMedia) libvlc_media_release(oldMedia);
                                    done->store(true);
                                }),
                                done, oldHost});
        } else {
            libvlc_media_player_stop(mp_);  // synchronous (shutdown/explicit stop)
            libvlc_media_player_release(mp_);
            mp_ = nullptr;
            if (media_) {
                libvlc_media_release(media_);
                media_ = nullptr;
            }
            const HWND oldHost = currentHost_.load();  // released synchronously -> host free now
            for (auto& h : hosts_)
                if (h.hwnd == oldHost) { h.busy = false; break; }
            currentHost_.store(nullptr);
        }
    } else if (media_) {
        libvlc_media_release(media_);
        media_ = nullptr;
    }
    playing_.store(false);
    {
        std::lock_guard<std::mutex> lk(statsMtx_);
        snapshot_ = FlowStats{};
    }
}

void VlcPlayer::doPlay(const Cmd& c) {
    doStop(/*async=*/true);  // tear the previous channel down off-thread — don't block this open
    if (!inst_) return;
    diag::info(L"play: " + c.url);
    const std::string u = utf8FromWide(c.url);
    libvlc_media_t* m = libvlc_media_new_location(inst_, u.c_str());
    if (!m) {
        diag::error(L"libvlc_media_new_location failed for: " + c.url);
        return;
    }
    libvlc_media_add_option(m, (":network-caching=" + std::to_string(cachingMs_.load())).c_str());
    if (!c.userAgent.empty())
        libvlc_media_add_option(m, (":http-user-agent=" + utf8FromWide(c.userAgent)).c_str());
    if (!c.referrer.empty())
        libvlc_media_add_option(m, (":http-referrer=" + utf8FromWide(c.referrer)).c_str());

    mp_ = libvlc_media_player_new_from_media(m);
    if (!mp_) {
        libvlc_media_release(m);
        diag::error(L"libvlc_media_player_new_from_media failed");
        return;
    }
    media_ = m;  // retain the media ref so sampleStats() can read its counters

    // Reset per-stream stats accumulators for a clean first delta.
    firstSample_ = true;
    prevReadBytes_ = prevDemuxBytes_ = 0;
    prevCorrupted_ = prevDiscontinuity_ = prevLostPictures_ = 0;
    prevDisplayedPictures_ = 0;
    savedAudioTrack_ = -1;  // fresh stream: the previous channel's audio-track id no longer applies

    // Attach to a proven-FREE vout host, never the pane HWND directly: reusing a surface whose
    // previous vout hasn't been released is what makes libVLC spawn its "VLC (Direct3D11 output)"
    // top-level window on rapid channel-surf. pickVoutHost() reuses a drained host or grows the
    // pool; it's guaranteed non-null in normal operation (the pane pre-creates two). video_ (the
    // pane HWND) is only a pathological last resort so a pool miss degrades to the old behaviour
    // rather than crashing.
    reapAsyncStops();  // return any hosts whose reaper just finished to the free set
    HWND host = pickVoutHost();
    if (!host) host = video_;
    if (host && IsWindow(host)) {
        currentHost_.store(host);  // the UI reads this on PlayerEvent::Playing to show this host
        libvlc_media_player_set_hwnd(mp_, static_cast<void*>(host));
        // Don't let libVLC's video window swallow mouse/keyboard input. We host our own
        // interactions over the surface (double-click fullscreen, and in video-only mode the
        // drag-to-move + right-click menu + Esc). The vout host's window proc forwards the clicks
        // it does receive to the pane's VideoProc, so the pane still gets them.
        libvlc_video_set_mouse_input(mp_, 0);
        libvlc_video_set_key_input(mp_, 0);
    }
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

// Worker-thread only. Reads libVLC's cumulative media stats, turns them into
// real per-second rates (byte-counter deltas over wall-clock) plus per-sample
// event deltas, publishes the snapshot, and nudges the UI to repaint the meter.
void VlcPlayer::sampleStats() {
    if (!mp_ || !media_) return;
    libvlc_media_stats_t s{};
    if (!libvlc_media_get_stats(media_, &s)) return;

    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastSampleTime_).count();
    if (dt < 0.001) dt = 0.001;

    FlowStats fs;
    fs.playing = playing_.load();
    // Snapshot (not a delta): data read off the network but not yet consumed by the
    // demux ≈ how much is buffered ahead of playback.
    fs.bufferedBytes = std::max(0LL, static_cast<long long>(s.i_read_bytes) -
                                         static_cast<long long>(s.i_demux_read_bytes));
    if (!firstSample_) {
        // Cumulative byte counters are 32-bit ints in libVLC 3.x and can wrap on
        // long streams; a negative delta means a wrap/reset — treat it as 0.
        const auto perSec = [dt](long long cur, long long prev) {
            const long long d = cur - prev;
            return d > 0 ? static_cast<double>(d) / dt : 0.0;
        };
        fs.demuxBytesPerSec = perSec(s.i_demux_read_bytes, prevDemuxBytes_);
        fs.readBytesPerSec = perSec(s.i_read_bytes, prevReadBytes_);
        fs.corruptedDelta = std::max(0, s.i_demux_corrupted - prevCorrupted_);
        fs.discontinuityDelta = std::max(0, s.i_demux_discontinuity - prevDiscontinuity_);
        fs.lostPicturesDelta = std::max(0, s.i_lost_pictures - prevLostPictures_);
        const int shownDelta = std::max(0, s.i_displayed_pictures - prevDisplayedPictures_);
        fs.displayedPerSec = shownDelta / dt;
    }
    prevDemuxBytes_ = s.i_demux_read_bytes;
    prevReadBytes_ = s.i_read_bytes;
    prevCorrupted_ = s.i_demux_corrupted;
    prevDiscontinuity_ = s.i_demux_discontinuity;
    prevLostPictures_ = s.i_lost_pictures;
    prevDisplayedPictures_ = s.i_displayed_pictures;
    lastSampleTime_ = now;
    firstSample_ = false;

    {
        std::lock_guard<std::mutex> lk(statsMtx_);
        snapshot_ = fs;
    }
    if (evtTarget_ && evtMsg_)
        PostMessageW(evtTarget_, evtMsg_,
                     MAKEWPARAM(static_cast<WORD>(PlayerEvent::Stats), static_cast<WORD>(tag_)), 0);
}

FlowStats VlcPlayer::flowStats() const {
    std::lock_guard<std::mutex> lk(statsMtx_);
    return snapshot_;
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
        case libvlc_MediaPlayerPlaying:
            ev = PlayerEvent::Playing;
            playing_.store(true);
            // The audio track list exists now — apply this pane's mute/volume (deselect the audio
            // track for a background pane, or select + set volume for the active one). Thread-safe
            // from the event thread (enqueues; the worker touches mp_).
            enqueue({Cmd::Mute});
            break;
        case libvlc_MediaPlayerPaused: ev = PlayerEvent::Paused; break;
        case libvlc_MediaPlayerStopped: ev = PlayerEvent::Stopped; playing_.store(false); break;
        case libvlc_MediaPlayerEndReached: ev = PlayerEvent::EndReached; playing_.store(false); break;
        case libvlc_MediaPlayerEncounteredError: ev = PlayerEvent::Error; playing_.store(false); break;
        default: return;
    }
    if (evtTarget_ && evtMsg_)
        PostMessageW(evtTarget_, evtMsg_, MAKEWPARAM(static_cast<WORD>(ev), static_cast<WORD>(tag_)),
                     lp);
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

void VlcPlayer::registerVoutHost(HWND host) {
    if (!host) return;
    Cmd c{Cmd::AddHost};
    c.pvalue = host;  // the worker adds it to hosts_ (worker-only) when it drains the queue
    enqueue(std::move(c));
}

void VlcPlayer::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    volume_.store(volume);
    Cmd c{Cmd::Volume};
    c.ivalue = volume;
    enqueue(std::move(c));
}

void VlcPlayer::setMuted(bool muted) {
    muted_.store(muted);
    enqueue({Cmd::Mute});  // applyAudioState() on the worker (un)selects the audio track
}

// Worker-thread only. Silence a background pane by DESELECTING its audio track rather than
// zeroing its volume: libVLC 3.x resets a player's output volume to 100% whenever it recreates
// the audio output (an HLS low->high quality switch, with NO event fired), so a volume-based mute
// leaks and pulses on adaptive feeds. A player with no audio track selected has no audio output at
// all, so there is nothing to reset. The active pane re-selects its track and applies its volume.
void VlcPlayer::applyAudioState() {
    if (!mp_) return;
    if (muted_.load()) {
        const int cur = libvlc_audio_get_track(mp_);
        if (cur != -1) {  // audio currently on -> remember which track, then deselect it
            savedAudioTrack_ = cur;
            libvlc_audio_set_track(mp_, -1);  // no audio ES selected -> no aout to leak/reset
            diag::info(L"pane " + std::to_wstring(tag_) + L" mute: audio track -1 (saved " +
                       std::to_wstring(savedAudioTrack_) + L")");
        }
    } else {
        // Only (re)select a track if audio is actually OFF, i.e. WE deselected it. If a track is
        // already selected, leave libVLC's choice alone — never override the user's preferred audio
        // language, and never touch track selection during ordinary (single-pane) playback.
        if (libvlc_audio_get_track(mp_) == -1) {
            int t = -1, first = -1;  // validate savedAudioTrack_ against THIS stream's tracks
            if (libvlc_track_description_t* d = libvlc_audio_get_track_description(mp_)) {
                for (auto* it = d; it; it = it->p_next) {
                    if (it->i_id == -1) continue;
                    if (first < 0) first = it->i_id;
                    if (it->i_id == savedAudioTrack_) t = savedAudioTrack_;  // saved track still present
                }
                libvlc_track_description_list_release(d);
            }
            if (t < 0) t = first;  // saved id absent (channel changed) -> first real audio track
            if (t >= 0) {
                libvlc_audio_set_track(mp_, t);
                diag::info(L"pane " + std::to_wstring(tag_) + L" unmute: audio track " +
                           std::to_wstring(t));
            }
        }
        libvlc_audio_set_volume(mp_, volume_.load());
    }
}

void VlcPlayer::setNetworkCaching(int ms) {
    cachingMs_.store(std::clamp(ms, 100, 60000));  // applied at the next media open
}

void VlcPlayer::setAspectRatio(const char* ar) {
    Cmd c{Cmd::Aspect};
    if (ar) c.svalue = ar;
    enqueue(std::move(c));
}

// ---- recording (headless second player, worker-thread only) ----------------

void VlcPlayer::doRecordStart(const Cmd& c) {
    doRecordStop();  // Phase 1: one recording at a time
    if (!inst_) return;
    const std::string u = utf8FromWide(c.url);
    libvlc_media_t* m = libvlc_media_new_location(inst_, u.c_str());
    if (!m) {
        diag::error(L"record: media_new failed for " + c.url);
        return;
    }
    // Stream-copy to a .ts file (no re-encode, low CPU). Forward slashes so the path
    // parses; single-quote the value so spaces survive; and DOUBLE any literal ' —
    // VLC's config-chain parser closes a single-quoted value on the first ' unless
    // it's doubled. The channel name is sanitized, but the %USERPROFILE% directory
    // may legitimately contain an apostrophe (e.g. C:\Users\O'Brien).
    std::wstring path = c.recPath;
    std::replace(path.begin(), path.end(), L'\\', L'/');
    std::string dst = utf8FromWide(path);
    for (size_t i = 0; (i = dst.find('\'', i)) != std::string::npos; i += 2) dst.insert(i, 1, '\'');
    const std::string mux = c.svalue.empty() ? "ts" : c.svalue;  // container (stream copy)
    libvlc_media_add_option(m, (":sout=#std{access=file,mux=" + mux + ",dst='" + dst + "'}").c_str());
    libvlc_media_add_option(m, ":sout-keep");
    libvlc_media_add_option(m, (":network-caching=" + std::to_string(cachingMs_.load())).c_str());
    if (!c.userAgent.empty())
        libvlc_media_add_option(m, (":http-user-agent=" + utf8FromWide(c.userAgent)).c_str());
    if (!c.referrer.empty())
        libvlc_media_add_option(m, (":http-referrer=" + utf8FromWide(c.referrer)).c_str());
    rec_ = libvlc_media_player_new_from_media(m);
    libvlc_media_release(m);
    if (!rec_) {
        diag::error(L"record: player_new failed");
        return;
    }
    if (libvlc_media_player_play(rec_) != 0) {  // headless (no set_hwnd) -> ES muxed to file
        diag::error(L"record: play failed to start");
        libvlc_media_player_release(rec_);
        rec_ = nullptr;
        return;
    }
    recording_.store(true);
    {
        std::lock_guard<std::mutex> lk(recMtx_);
        recFile_ = c.recPath;
    }
    diag::info(L"recording started -> " + c.recPath);
}

void VlcPlayer::doRecordStop(bool async) {
    if (rec_) {
        if (async) {
            // Hand the (blocking) recorder stop()/release() to a reaper thread, exactly like
            // doStop(async) does for playback, so a mode-switch teardown's worker join can't
            // block the UI thread on a stuck recording feed. recording_ is cleared right away;
            // the .ts is finalized when the reaper's stop() returns (TS needs no trailer, so a
            // late finalize is safe). No media ref to release — doRecordStart drops it at open.
            libvlc_media_player_t* old = rec_;
            rec_ = nullptr;
            reapAsyncStops();  // drop any that have already finished
            auto done = std::make_shared<std::atomic<bool>>(false);
            reapers_.push_back({std::thread([old, done] {
                                    libvlc_media_player_stop(old);
                                    libvlc_media_player_release(old);
                                    done->store(true);
                                }),
                                done});
        } else {
            libvlc_media_player_stop(rec_);  // synchronous: flushes + finalizes the .ts (TS needs no trailer)
            libvlc_media_player_release(rec_);
            rec_ = nullptr;
        }
        diag::info(L"recording stopped");
    }
    recording_.store(false);
    {
        std::lock_guard<std::mutex> lk(recMtx_);
        recFile_.clear();
    }
}

bool VlcPlayer::startRecording(const std::wstring& url, const std::wstring& userAgent,
                               const std::wstring& referrer, const std::wstring& filePath,
                               const std::string& mux) {
    if (!inst_) return false;
    Cmd c{Cmd::RecordStart};
    c.url = url;
    c.userAgent = userAgent;
    c.referrer = referrer;
    c.recPath = filePath;
    c.svalue = mux;  // container: ts | mkv
    enqueue(std::move(c));
    return true;
}

void VlcPlayer::stopRecording() { enqueue({Cmd::RecordStop}); }

std::wstring VlcPlayer::recordingFile() const {
    std::lock_guard<std::mutex> lk(recMtx_);
    return recFile_;
}

}  // namespace rabbitears
