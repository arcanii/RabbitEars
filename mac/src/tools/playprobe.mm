// SPDX-License-Identifier: GPL-3.0-or-later
//
// playprobe — a headless libVLC smoke test for the macOS build. Verifies that
// libVLC loads (plugins resolve) and can open/play a stream WITHOUT any video or
// audio output device, so it runs in CI / over SSH. Exits 0 if it reaches the
// Playing state, 3 on Error, 2 if it never starts, 1 if libVLC won't initialize.
//
//   RabbitEarsPlayProbe [stream-url] [seconds]
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include <vlc/vlc.h>

namespace {
const char* stateName(libvlc_state_t s) {
    switch (s) {
        case libvlc_NothingSpecial: return "NothingSpecial";
        case libvlc_Opening:        return "Opening";
        case libvlc_Buffering:      return "Buffering";
        case libvlc_Playing:        return "Playing";
        case libvlc_Paused:         return "Paused";
        case libvlc_Stopped:        return "Stopped";
        case libvlc_Ended:          return "Ended";
        case libvlc_Error:          return "Error";
        default:                    return "?";
    }
}
}  // namespace

int main(int argc, const char** argv) {
    @autoreleasepool {
        // Apple's public HLS reference stream — stable, good for a reachability probe.
        const char* url = argc > 1 ? argv[1]
            : "https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_ts/master.m3u8";
        const int seconds = argc > 2 ? atoi(argv[2]) : 12;

#if defined(RABBITEARS_VLC_PLUGIN_PATH)
        if (!getenv("VLC_PLUGIN_PATH")) setenv("VLC_PLUGIN_PATH", RABBITEARS_VLC_PLUGIN_PATH, 1);
#endif
        const char* args[] = {"--no-video", "--no-audio", "--no-video-title-show"};
        libvlc_instance_t* vlc = libvlc_new(sizeof(args) / sizeof(args[0]), args);
        if (!vlc) {
            printf("[playprobe] libvlc_new FAILED — plugins not found? "
                   "(VLC_PLUGIN_PATH=%s)\n", getenv("VLC_PLUGIN_PATH") ?: "(unset)");
            return 1;
        }
        printf("[playprobe] libVLC %s — opening %s\n", libvlc_get_version(), url);

        libvlc_media_t* media = libvlc_media_new_location(vlc, url);
        libvlc_media_player_t* mp = libvlc_media_player_new_from_media(media);
        libvlc_media_release(media);
        libvlc_media_player_play(mp);

        libvlc_state_t st = libvlc_NothingSpecial;
        for (int i = 0; i < seconds * 2; ++i) {
            usleep(500000);
            st = libvlc_media_player_get_state(mp);
            if (st == libvlc_Playing || st == libvlc_Error || st == libvlc_Ended) break;
        }
        printf("[playprobe] final state = %s\n", stateName(st));

        libvlc_media_player_stop(mp);
        libvlc_media_player_release(mp);
        libvlc_release(vlc);
        return st == libvlc_Playing ? 0 : (st == libvlc_Error ? 3 : 2);
    }
}
