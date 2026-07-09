// SPDX-License-Identifier: GPL-3.0-or-later
//
// VlcEngineMac — owns the single shared libVLC instance (the mac peer of Win32's
// VlcEngine). libvlc_new loads ~325 plugins, so it must happen ONCE per process; a
// libVLC instance then backs any number of media players. Every VlcPlayerMac borrows
// this handle via VlcPlayerMac::init(engine) and never creates or releases it. This is
// the seam multi-view (Split/2×2) and PiP are built on — an Nth pane's player is cheap
// because they share this instance.
//
// Lifetime: init() one engine up front, hand it to each player's init(). On teardown,
// destroy ALL players first (they stop/release media players that belong to this
// instance), THEN let the engine release it.
#pragma once

struct libvlc_instance_t;

namespace rabbitears {

class VlcEngineMac {
public:
    VlcEngineMac() = default;
    ~VlcEngineMac();
    VlcEngineMac(const VlcEngineMac&) = delete;
    VlcEngineMac& operator=(const VlcEngineMac&) = delete;

    // Create the shared instance (idempotent). Returns true if it is ready. Also sets
    // VLC_PLUGIN_PATH to the bundled plugins (or the dev VLC.app) if not already set.
    bool init();
    // Release the shared instance (idempotent). Every VlcPlayerMac built on this engine
    // must already be destroyed.
    void shutdown();

    bool isReady() const { return inst_ != nullptr; }
    // The borrowed handle players pass to libVLC. nullptr until init() succeeds (or when
    // libVLC isn't provisioned — the app then runs in a safe no-backend stub mode).
    libvlc_instance_t* handle() const { return inst_; }

private:
    libvlc_instance_t* inst_ = nullptr;
};

}  // namespace rabbitears
