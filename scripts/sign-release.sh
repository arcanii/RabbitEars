#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sign a RabbitEars Windows release on macOS with the family Ed25519 key, using
# Sparkle's `sign_update` (the same tool + login-Keychain key you use to sign the
# macOS apps). Prints the sparkle:edSignature to feed into make-appcast.ps1.
#
# Usage:
#   scripts/sign-release.sh path/to/RabbitEars-0.1.1-setup.exe
#
# Notes:
#   * Copy the installer built on Windows (build\installer\RabbitEars-<ver>-setup.exe)
#     to the Mac first. The signature covers the exact bytes, so upload that SAME
#     file to the GitHub release.
#   * sign_update reads the private key from the login Keychain; macOS may prompt to
#     allow access — click Allow. The family key is stored under the Keychain account
#     "SQLTerminal", so --account SQLTerminal is passed by default. SIGN_UPDATE_ARGS
#     replaces that default — e.g. to sign with a key FILE instead:
#         SIGN_UPDATE_ARGS="-f /path/to/private_key" scripts/sign-release.sh <file>
#     (SIGN_UPDATE_ARGS="" passes no arguments at all — sign_update's DEFAULT key, with a warning.)
#   * sign_update is looked for in ./bin, then in this repo's mac build dir
#     (build-mac*/sparkle/bin — the mac build fetches Sparkle there), then on PATH.
#     Anywhere else, point at it:
#         SIGN_UPDATE=/path/to/Sparkle/bin/sign_update scripts/sign-release.sh <file>
#   * Quote a path with spaces or accents: scripts/sign-release.sh "/Volumes/…/RabbitEars-0.2.19-setup.exe"
#   * stdout is ONLY the base64 signature, so you can capture it:
#         SIG=$(scripts/sign-release.sh RabbitEars-0.1.1-setup.exe)

set -euo pipefail

file="${1:-}"
if [[ -z "$file" || ! -f "$file" ]]; then
  echo "usage: $(basename "$0") <path-to-RabbitEars-x.y.z-setup.exe>" >&2
  exit 1
fi

# The repo root, through any symlink to this script (macOS readlink has no -f) and whatever
# CDPATH says (an exported CDPATH makes `cd` print, which would corrupt the capture).
src="$0"
while [[ -L "$src" ]]; do
  dir="$(CDPATH= cd -- "$(dirname -- "$src")" && pwd)"
  src="$(readlink "$src")"
  [[ "$src" == /* ]] || src="$dir/$src"
done
repo="$(CDPATH= cd -- "$(dirname -- "$src")/.." && pwd)"

# Locate Sparkle's sign_update: ./bin, the repo's mac build dir (build-mac itself before any
# build-mac-* sibling, which may be a stale tree), then PATH.
tool="${SIGN_UPDATE:-}"
if [[ -z "$tool" ]]; then
  for c in ./bin/sign_update "$repo/build-mac/sparkle/bin/sign_update" \
           "$repo"/build-mac*/sparkle/bin/sign_update \
           "$(command -v sign_update 2>/dev/null || true)"; do
    if [[ -n "$c" && -x "$c" ]]; then tool="$c"; break; fi
  done
fi
if [[ -z "$tool" ]]; then
  echo "error: sign_update not found. Set SIGN_UPDATE=/path/to/sign_update" >&2
  echo "       (looked in ./bin, $repo/build-mac*/sparkle/bin and on PATH; it ships in" >&2
  echo "        the Sparkle release under bin/ — the same tool that signs the macOS apps)." >&2
  exit 1
fi

# The family key's Keychain account, unless SIGN_UPDATE_ARGS says otherwise (set, even empty).
args="${SIGN_UPDATE_ARGS-"--account SQLTerminal"}"
case " $args " in
  *" --account "* | *" -f "*) ;;
  *) echo "warning: no --account (or -f key file) in SIGN_UPDATE_ARGS — sign_update will use its" >&2
     echo "         DEFAULT Keychain key, not the family key. A wrong-key signature fails only" >&2
     echo "         on users' machines: verify it against Win32/platform/Updater.cpp's public key" >&2
     echo "         (docs/RELEASING.md) before publishing." >&2 ;;
esac

echo "Signing $file" >&2
echo "  with $tool${args:+ $args}" >&2
# shellcheck disable=SC2086
out="$("$tool" $args "$file")"
echo "  -> $out" >&2

# Sparkle prints:  sparkle:edSignature="BASE64==" length="12345"
sig="$(printf '%s\n' "$out" | sed -n 's/.*sparkle:edSignature="\([^"]*\)".*/\1/p')"
if [[ -z "$sig" ]]; then
  echo "error: could not parse edSignature from sign_update output above." >&2
  exit 1
fi

echo >&2
echo "Signature (paste this back / into make-appcast.ps1 -Signature):" >&2
printf '%s\n' "$sig"
