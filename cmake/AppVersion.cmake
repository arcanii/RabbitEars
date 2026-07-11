# SPDX-License-Identifier: GPL-3.0-or-later
#
# Marketing version, included by BOTH build systems (root CMakeLists.txt for Windows,
# mac/CMakeLists.txt for macOS). The per-commit BUILD_NUMBER (git rev-list count) is
# computed separately in each; only this dotted version is shared.
#
# Bump on release (the git tag + appcast still gate the actual rollout).
set(APP_VERSION "0.2.8")

# macOS carries its own decoupled version: the mac port still lacks the Windows theme
# engine, so the platforms are NOT lockstep on patch numbers. This is the one intentional
# exception to the "both platforms share one version" rule — the mac build (APPLE)
# overrides it here, feeding both CFBundleShortVersionString and the generated version.h.
# Windows (the .exe / windows-core CI) is unaffected and stays on its own 0.2.x line.
#
# mac 0.2.7 continues the parity line: 0.2.0 landed multi-view (Split/2x2), Picture-in-Picture
# and the TV Guide (EPG); 0.2.7 adds the Windows 0.2.6/0.2.7 parity set — favourites import/export
# + Show-in-Guide, PiP inset resize/persist, named saved layouts, per-pane recording, and the
# recording scheduler with EPG series rules. (mac history: 0.1.7 → 0.1.10 → 0.2.0 → 0.2.7.)
if(APPLE)
  set(APP_VERSION "0.2.7")
endif()
