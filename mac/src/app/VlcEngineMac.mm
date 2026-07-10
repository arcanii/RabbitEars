// SPDX-License-Identifier: GPL-3.0-or-later
// See VlcEngineMac.h. libVLC calls are compiled only when RABBITEARS_HAVE_LIBVLC is
// defined (Mac.cmake sets it once libVLC is provisioned); otherwise init() is a safe
// no-op returning false, so the app still builds/runs without a backend. MRC-style —
// no ARC needed (the NSBundle/NSString lookups are autoreleased, nothing retained).
#import "VlcEngineMac.h"

#import <Foundation/Foundation.h>

#include <cstdlib>

#include "platform/Log.h"

#if defined(RABBITEARS_HAVE_LIBVLC)
#include <vlc/vlc.h>
#endif

namespace rabbitears {

VlcEngineMac::~VlcEngineMac() { shutdown(); }

bool VlcEngineMac::init() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (inst_) return true;  // idempotent
    // Point libVLC at its plugins unless VLC_PLUGIN_PATH is already set. Prefer the
    // plugins bundled in the app (Contents/PlugIns — a self-contained release); fall
    // back to the compile-time VLC.app path for non-bundled dev builds. (Moved here
    // from the old per-player ctor so it runs exactly once, with the shared instance.)
    if (!getenv("VLC_PLUGIN_PATH")) {
        NSString* bundled = NSBundle.mainBundle.builtInPlugInsPath;  // Contents/PlugIns
        BOOL isDir = NO;
        if (bundled.length &&
            [NSFileManager.defaultManager fileExistsAtPath:bundled isDirectory:&isDir] && isDir) {
            setenv("VLC_PLUGIN_PATH", bundled.fileSystemRepresentation, 1);
        }
#if defined(RABBITEARS_VLC_PLUGIN_PATH)
        else {
            setenv("VLC_PLUGIN_PATH", RABBITEARS_VLC_PLUGIN_PATH, 1);
        }
#endif
    }
    const char* args[] = {"--no-video-title-show"};
    inst_ = libvlc_new(sizeof(args) / sizeof(args[0]), args);
    diag::info(inst_ ? L"libVLC shared instance created"
                     : L"libVLC init FAILED (plugins/deps?)");
    return inst_ != nullptr;
#else
    diag::info(L"VlcEngineMac::init (stub: libVLC not provisioned)");
    return false;
#endif
}

void VlcEngineMac::shutdown() {
#if defined(RABBITEARS_HAVE_LIBVLC)
    if (inst_) {
        libvlc_release(inst_);
        inst_ = nullptr;
    }
#endif
}

}  // namespace rabbitears
