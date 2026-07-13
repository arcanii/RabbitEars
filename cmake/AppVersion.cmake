# SPDX-License-Identifier: GPL-3.0-or-later
#
# Marketing version, included by BOTH build systems (root CMakeLists.txt for Windows,
# mac/CMakeLists.txt for macOS). The per-commit BUILD_NUMBER (git rev-list count) is
# computed separately in each; only this dotted version is shared.
#
# Bump on release (the git tag + appcast still gate the actual rollout).
set(APP_VERSION "0.2.10")

# macOS carries its own decoupled version: the mac port still lacks the Windows theme
# engine, so the platforms are NOT lockstep on patch numbers. This is the one intentional
# exception to the "both platforms share one version" rule — the mac build (APPLE)
# overrides it here, feeding both CFBundleShortVersionString and the generated version.h.
# Windows (the .exe / windows-core CI) is unaffected and stays on its own 0.2.x line.
#
# mac 0.2.8 reaches Windows 0.2.8 parity: 0.2.0 landed multi-view + the TV Guide (EPG); 0.2.7 added
# the 0.2.6/0.2.7 set (favourites I/O, PiP resize/persist, saved layouts, per-pane recording, the
# recording scheduler + EPG series rules); 0.2.8 adds localization — English + 日本語 via the shared
# common/i18n catalog + a language selector. (mac history: 0.1.7 → 0.1.10 → 0.2.0 → 0.2.7 → 0.2.8.)
if(APPLE)
  set(APP_VERSION "0.2.8")
endif()
