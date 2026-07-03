#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Configure + build the macOS app (shared core + self-test) and run the self-test.
# The peer of scripts/build.cmd (Windows). Drives the unified root CMake, which
# dispatches to common/ + mac/ on macOS. See docs/MACOS_PORT.md.
#
#   scripts/build-mac.sh              # core lib + self-test
#   scripts/build-mac.sh --app        # also build the RabbitEars.app (needs deps)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-mac"

CMAKE_ARGS=(-S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=RelWithDebInfo)
if [[ "${1:-}" == "--app" ]]; then
  CMAKE_ARGS+=(-DRABBITEARS_BUILD_MAC=ON)
  shift
fi

cmake "${CMAKE_ARGS[@]}" "$@"
cmake --build "${BUILD_DIR}"

echo
echo "== core self-test =="
ctest --test-dir "${BUILD_DIR}" --output-on-failure
