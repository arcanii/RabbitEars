// SPDX-License-Identifier: GPL-3.0-or-later
// VlcPlayer — thin RAII wrapper over libVLC for embedded IPTV playback. Renders
// into a child HWND via libvlc_media_player_set_hwnd (called before play, or
// libVLC opens its own top-level output window). libVLC event callbacks run on a
// libVLC thread, so they are marshaled to the UI thread via PostMessage.
//
// libVLC headers are included only in the .cpp; the handle types are forward-
// declared here so including VlcPlayer.h stays cheap.
#pragma once

#include <string>

#include <windows.h>

struct libvlc_instance_t;
struct libvlc_media_player_t;

namespace rabbitears {

// Playback state, posted to the event target's message (as wParam).
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

    // Create the libVLC instance. Returns false (and the app should degrade) if
    // the libVLC runtime could not be loaded.
    bool init();
    bool isReady() const { return inst_ != nullptr; }

    // Where playback events are posted: PostMessage(target, msg, (WPARAM)PlayerEvent, lParam).
    void setEventTarget(HWND target, UINT msg) { evtTarget_ = target; evtMsg_ = msg; }

    // Render into `video` (a WS_CHILD window). Call before play().
    void attach(HWND video) { video_ = video; }

    // Start playing a stream. Optional per-channel HTTP hints become libVLC media
    // options (:http-user-agent / :http-referrer).
    bool play(const std::wstring& url, const std::wstring& userAgent = {},
              const std::wstring& referrer = {});
    void togglePause();
    void stop();

    void setVolume(int volume);            // 0..100
    int volume() const { return volume_; }
    void setAspectRatio(const char* ar);   // "16:9" / "4:3" / nullptr (default)
    bool isPlaying() const;

private:
    void destroyPlayer();

    libvlc_instance_t*     inst_ = nullptr;
    libvlc_media_player_t* mp_ = nullptr;
    HWND                   video_ = nullptr;
    HWND                   evtTarget_ = nullptr;
    UINT                   evtMsg_ = 0;
    int                    volume_ = 80;
};

}  // namespace rabbitears
