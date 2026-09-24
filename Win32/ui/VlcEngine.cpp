// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VlcEngine.h"

#include <cstdarg>
#include <cstdio>
#include <string>

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
    // libVLC's own warnings/errors map onto our severities, so they honour the log level too —
    // otherwise "Error" would still be noisy with VLC-WARN. Checked BEFORE formatting: this runs
    // on a libVLC thread and can fire often on a sick stream.
    const diag::Level lvl = level >= LIBVLC_ERROR ? diag::Level::Error : diag::Level::Warn;
    if (!diag::enabled(lvl)) return;
    // Sized to the message (up to 64 KB), not a fixed 1 KB: a line cut mid-URL can end partway
    // through a stream path's login, and a partial one is neither a registered secret nor a URL
    // shape the log's masking recognises (platform/UrlRedact.h). (A cut query value is still
    // masked — by its parameter's name.)
    va_list ap;
    va_copy(ap, args);
    const int need = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    std::string buf(static_cast<size_t>(need < 65536 ? need : 65536) + 1, '\0');
    va_copy(ap, args);
    vsnprintf(buf.data(), buf.size(), fmt, ap);
    va_end(ap);
    buf.resize(buf.size() - 1);  // drop the terminator vsnprintf wrote
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
