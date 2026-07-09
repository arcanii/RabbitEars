// SPDX-License-Identifier: GPL-3.0-or-later
// VlcEngine — owns the single shared libVLC instance.
//
// libvlc_new loads ~325 plugins (the ~10 s the splash covers), so it must happen
// ONCE per process. A libVLC instance, however, backs any number of media players.
// VlcEngine owns that instance; every VlcPlayer (today: playback + the headless
// recorder; next: split-view panes and PIP) borrows the handle via
// VlcPlayer::init(engine) and never creates or releases it. This is the seam the
// multi-player roadmap (multiple simultaneous views / PIP / concurrent recording)
// is built on — adding an Nth player is now cheap because they share this instance.
//
// Lifetime: init() one engine up front, hand it to each player's init(). On
// teardown, shut down ALL players first (so their worker/reaper threads have
// finished touching the instance), THEN shutdown() the engine.
#pragma once

struct libvlc_instance_t;

namespace rabbitears {

class VlcEngine {
public:
    VlcEngine() = default;
    ~VlcEngine();
    VlcEngine(const VlcEngine&) = delete;
    VlcEngine& operator=(const VlcEngine&) = delete;

    // Create the shared instance + route libVLC's own warnings/errors to the diag
    // file. Idempotent; returns true if the instance is ready. Safe to call again.
    bool init();
    // Release the shared instance (idempotent). Every VlcPlayer built on this engine
    // must already be shut down — their reaper threads stop/release media players that
    // belong to this instance.
    void shutdown();

    bool isReady() const { return inst_ != nullptr; }
    // The borrowed handle players pass to libVLC. nullptr until init() succeeds.
    libvlc_instance_t* handle() const { return inst_; }

private:
    libvlc_instance_t* inst_ = nullptr;
};

}  // namespace rabbitears
