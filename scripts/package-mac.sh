#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Make a built RabbitEars.app self-contained + signed — the mac peer of the Windows
# build-installer/sign steps. Bundles libVLC (libvlc + libvlccore into
# Contents/Frameworks, the plugins tree into Contents/PlugIns), drops the
# /Applications/VLC.app rpath so the app loads its OWN libVLC, then codesigns
# inside-out (Developer ID if given, else ad-hoc so it still runs on Apple Silicon).
#
#   scripts/package-mac.sh build-mac/mac/RabbitEars.app \
#       --sign "Developer ID Application: Name (TEAMID)" [--vlc /Applications/VLC.app]
#
# --entitlements defaults to mac/packaging/RabbitEars.entitlements whenever --sign is
# given; see the guard below for why signing without it must not be possible.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_ent="$here/../mac/packaging/RabbitEars.entitlements"

app=""; ID=""; ENT=""; vlc="/Applications/VLC.app"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sign)         ID="${2:-}"; shift 2;;
    --entitlements) ENT="${2:-}"; shift 2;;
    --vlc)          vlc="${2:-}"; shift 2;;
    -*)             echo "unknown option: $1" >&2; exit 1;;
    *)              app="$1"; shift;;
  esac
done
[[ -d "$app" ]] || { echo "usage: package-mac.sh <RabbitEars.app> [--sign <id>] [--vlc <VLC.app>] [--entitlements <plist>]" >&2; exit 1; }

# A Developer ID signature turns on the hardened runtime (below), and the hardened
# runtime denies audio capture unless the entitlements grant it — SILENTLY: the Core
# Audio process tap, its aggregate device and the IOProc all return noErr and just
# deliver zeros, so the Spectrum meter sits flat with nothing in the log and macOS
# never prompts. Forgetting --entitlements therefore ships a plausible-looking,
# fully notarized build with a dead meter. Default it, and refuse to sign a hardened
# binary whose entitlements are missing or lack the audio-input key.
if [[ -n "$ID" ]]; then
  [[ -n "$ENT" ]] || ENT="$default_ent"
  [[ -f "$ENT" ]] || { echo "error: entitlements not found: $ENT" >&2; exit 1; }
  grep -q 'com.apple.security.device.audio-input' "$ENT" || {
    echo "error: $ENT lacks com.apple.security.device.audio-input — the Spectrum meter would be silently dead" >&2
    exit 1
  }
fi

libdir="$vlc/Contents/MacOS/lib"
plugdir="$vlc/Contents/MacOS/plugins"
[[ -d "$libdir" && -d "$plugdir" ]] || { echo "error: no libVLC under $vlc" >&2; exit 1; }
fw="$app/Contents/Frameworks"
pl="$app/Contents/PlugIns"
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"

echo "== bundle libVLC (from $vlc) =="
mkdir -p "$fw" "$pl"
cp -a "$libdir/"libvlc*.dylib "$libdir/"libvlccore*.dylib "$fw/"
cp -a "$plugdir/." "$pl/"
# Load libvlc from Contents/Frameworks (that rpath already exists for Sparkle),
# not from the build machine's VLC.app — makes the app self-contained.
if otool -l "$exe" | awk '/LC_RPATH/{f=1} f&&/ path /{print $2; f=0}' | grep -qxF "$libdir"; then
  install_name_tool -delete_rpath "$libdir" "$exe"
fi
echo "   Frameworks: $(ls "$fw" | tr '\n' ' ')"
echo "   PlugIns:    $(ls "$pl"/*.dylib 2>/dev/null | wc -l | tr -d ' ') plugins"
echo "   rpaths:     $(otool -l "$exe" | awk '/LC_RPATH/{f=1} f&&/ path /{print $2; f=0}' | tr '\n' ' ')"

signid="${ID:--}"
# Developer ID → hardened runtime + secure timestamp (both required for
# notarization); ad-hoc → neither (ad-hoc signatures can't be timestamped).
opts=(--force --sign "$signid")
[[ -n "$ID" ]] && opts+=(--options runtime --timestamp)
echo "== codesign (${ID:-ad-hoc}${ID:+ + hardened runtime}) =="
# Sparkle ships signed WITHOUT a secure timestamp (which notarization rejects), so
# re-sign its nested helpers inside-out with our identity — preserving the XPC /
# updater entitlements (e.g. the Downloader's network.client) — then the framework.
SP="$fw/Sparkle.framework"
if [[ -d "$SP" ]]; then
  SPV="$SP/Versions/B"; [[ -d "$SPV" ]] || SPV="$SP/Versions/Current"
  for x in "$SPV/XPCServices/"*.xpc "$SPV/Updater.app"; do
    [[ -e "$x" ]] && codesign "${opts[@]}" --preserve-metadata=entitlements "$x"
  done
  [[ -e "$SPV/Autoupdate" ]] && codesign "${opts[@]}" "$SPV/Autoupdate"
  codesign "${opts[@]}" "$SP"
fi
# Bundled VLC dylibs + plugins, then the app bundle last (which carries the
# entitlements). All same-identity so hardened-runtime library validation passes.
find "$fw" "$pl" -name "*.dylib" -type f -print0 | xargs -0 codesign "${opts[@]}"

# libVLC's plugin cache, rebuilt for THESE plugins. The plugins.dat copied from VLC.app records each
# plugin's size and mtime — and signing the dylibs (above) changes both, so every entry was "stale":
# libVLC dlopened all ~335 plugins on every launch (~0.85 s warm; ~80 s measured on a first launch,
# where each first dlopen is also malware-scanned) and printed a stale warning per plugin. Built here,
# AFTER the dylibs are signed and BEFORE the bundle is sealed, by the bundled libvlc itself
# (--reset-plugins-cache against VLC_PLUGIN_PATH writes that directory's plugins.dat; the entries are
# relative, so the cache holds once the app is copied to /Applications). With it libvlc_new takes
# ~10 ms. Not VLC.app's own binary for this: it would also rewrite VLC.app's signed cache.
echo "== libVLC plugin cache =="
cgdir="$(mktemp -d)"
cat > "$cgdir/vlc-cache-gen.c" <<'CEOF'
#include <stdio.h>
#include <string.h>
typedef struct libvlc_instance_t libvlc_instance_t;
libvlc_instance_t* libvlc_new(int argc, const char* const* argv);
void libvlc_release(libvlc_instance_t* p);
int main(int argc, char** argv) {
    // --reset: rebuild VLC_PLUGIN_PATH's plugins.dat.  (no arg): load it verbosely, for the check below.
    const int reset = argc > 1 && strcmp(argv[1], "--reset") == 0;
    const char* args[] = {reset ? "--quiet" : "-vv", "--reset-plugins-cache"};
    libvlc_instance_t* vlc = libvlc_new(reset ? 2 : 1, args);
    if (!vlc) { fprintf(stderr, "libvlc_new failed\n"); return 1; }
    libvlc_release(vlc);
    return 0;
}
CEOF
clang -O2 -o "$cgdir/vlc-cache-gen" "$cgdir/vlc-cache-gen.c" "$fw/libvlc.dylib" -Wl,-rpath,"$fw"
rm -f "$pl/plugins.dat"
VLC_PLUGIN_PATH="$pl" "$cgdir/vlc-cache-gen" --reset
[[ -s "$pl/plugins.dat" ]] || { echo "error: libVLC wrote no plugins.dat into $pl" >&2; exit 1; }
stale=$(VLC_PLUGIN_PATH="$pl" "$cgdir/vlc-cache-gen" 2>&1 | grep -c 'stale plugins cache' || true)
[[ "$stale" == 0 ]] || { echo "error: the rebuilt plugin cache still has $stale stale entries" >&2; exit 1; }
rm -rf "$cgdir"
# Signed like the plugins beside it: codesign treats everything under Contents/PlugIns as code, so an
# unsigned plugins.dat fails the deep verify below. (The copy from VLC.app carried VideoLAN's own
# signature in its extended attributes — that is why it passed before.)
codesign "${opts[@]}" "$pl/plugins.dat"
echo "   plugins.dat: $(stat -f %z "$pl/plugins.dat") bytes, 0 stale, signed"

codesign "${opts[@]}" ${ENT:+--entitlements "$ENT"} "$app"
codesign --verify --deep --strict "$app" && echo "   verify: OK"
echo "== packaged: $app =="
