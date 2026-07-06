// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VlcEngine.h"

#include <cstdarg>
#include <cstdio>

#include <vlc/vlc.h>

#include "platform/Encoding.h"
#include "platform/Log.h"

namespace rabbitears {
namespace {

// libVLC's own log, routed into our diagnostic file (warnings + errors only, so it
// stays small). Runs on libVLC threads — diag::write is thread-safe. Set once on the
// instance, so it takes no opaque pointer (the callback ignores it): the instance
// outlives every individual player, so there is no per-player state to key on here.
void vlcLogCb(void*, int level, const libvlc_log_t*, const char* fmt, va_list args) {
    if (level < LIBVLC_WARNING) return;
    char buf[1024];
    va_list ap;
    va_copy(ap, args);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag::write(level >= LIBVLC_ERROR ? L"VLC-ERR" : L"VLC-WARN", wideFromUtf8(buf));
}

}  // namespace

bool VlcEngine::init() {
    if (inst_) return true;
    // NB: stats collection is left ENABLED (no "--no-stats") so players can sample real
    // throughput + packet health per media and drive the buffer meter. "--quiet" is
    // dropped so libVLC emits its warning/error log, which we route to the diagnostic
    // file via libvlc_log_set (invaluable for stream triage).
    const char* args[] = {
        "--intf=dummy",       "--no-video-title-show", "--no-osd",
        "--network-caching=1000", "--http-reconnect",
    };
    inst_ = libvlc_new(static_cast<int>(sizeof(args) / sizeof(args[0])), args);
    if (!inst_) {
        diag::error(L"libVLC init failed (libvlc_new returned null)");
        return false;
    }
    libvlc_log_set(inst_, vlcLogCb, nullptr);
    diag::info(L"libVLC " + wideFromUtf8(libvlc_get_version()) + L" initialized");
    return true;
}

void VlcEngine::shutdown() {
    if (!inst_) return;
    libvlc_log_unset(inst_);
    libvlc_release(inst_);
    inst_ = nullptr;
}

VlcEngine::~VlcEngine() { shutdown(); }

}  // namespace rabbitears
