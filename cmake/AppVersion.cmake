# SPDX-License-Identifier: GPL-3.0-or-later
#
# Single source of truth for the marketing version, included by BOTH build
# systems (root CMakeLists.txt for Windows, mac/CMakeLists.txt for macOS) so the
# two platforms can never drift. The per-commit BUILD_NUMBER (git rev-list count)
# is computed separately in each; only this dotted version is shared.
#
# Bump this on release (the git tag + appcast still gate the actual rollout).
set(APP_VERSION "0.1.8")
