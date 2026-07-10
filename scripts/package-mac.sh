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
codesign "${opts[@]}" ${ENT:+--entitlements "$ENT"} "$app"
codesign --verify --deep --strict "$app" && echo "   verify: OK"
echo "== packaged: $app =="
