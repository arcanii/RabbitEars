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
#   * sign_update reads the private key from the login Keychain by default; macOS
#     may prompt to allow access — click Allow. To sign with a key FILE instead:
#         SIGN_UPDATE_ARGS="-f /path/to/private_key" scripts/sign-release.sh <file>
#   * If sign_update isn't on PATH or ./bin, point at it:
#         SIGN_UPDATE=/path/to/Sparkle/bin/sign_update scripts/sign-release.sh <file>
#   * stdout is ONLY the base64 signature, so you can capture it:
#         SIG=$(scripts/sign-release.sh RabbitEars-0.1.1-setup.exe)

set -euo pipefail

file="${1:-}"
if [[ -z "$file" || ! -f "$file" ]]; then
  echo "usage: $(basename "$0") <path-to-RabbitEars-x.y.z-setup.exe>" >&2
  exit 1
fi

# Locate Sparkle's sign_update.
tool="${SIGN_UPDATE:-}"
if [[ -z "$tool" ]]; then
  for c in ./bin/sign_update "$(command -v sign_update 2>/dev/null || true)"; do
    if [[ -n "$c" && -x "$c" ]]; then tool="$c"; break; fi
  done
fi
if [[ -z "$tool" ]]; then
  echo "error: sign_update not found. Set SIGN_UPDATE=/path/to/sign_update" >&2
  echo "       (it ships in the Sparkle release under bin/ — the same tool you use" >&2
  echo "        to sign the macOS apps)." >&2
  exit 1
fi

echo "Signing $file" >&2
echo "  with $tool" >&2
# shellcheck disable=SC2086
out="$("$tool" ${SIGN_UPDATE_ARGS:-} "$file")"
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
