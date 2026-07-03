#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generate a RabbitEars.xcodeproj for Xcode's debugger / Instruments, driven by the
# SAME CMake as scripts/build-mac.sh — CMakeLists.txt stays the single source of
# truth. build-xcode/ is a throwaway (gitignored); regenerate anytime.
#
# Editing SOURCE in Xcode edits the real files; build SETTINGS come from CMake, not
# the Xcode UI (re-run this after changing CMakeLists.txt or adding files). Needs
# VLC.app for libVLC (auto-detected by mac/cmake/Mac.cmake).
#
#   scripts/xcode.sh            # generate + open build-xcode/RabbitEars.xcodeproj
#   scripts/xcode.sh --no-open  # just (re)generate
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-xcode"

open_proj=1
args=()
for a in "$@"; do
  if [[ "$a" == "--no-open" ]]; then open_proj=0; else args+=("$a"); fi
done

cmake -G Xcode -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DRABBITEARS_BUILD_MAC=ON "${args[@]}"
[[ "$open_proj" == 1 ]] && open "${BUILD_DIR}/RabbitEars.xcodeproj"
echo "Xcode project: ${BUILD_DIR}/RabbitEars.xcodeproj"
