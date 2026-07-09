# SPDX-License-Identifier: GPL-3.0-or-later
#
# Marketing version, included by BOTH build systems (root CMakeLists.txt for Windows,
# mac/CMakeLists.txt for macOS). The per-commit BUILD_NUMBER (git rev-list count) is
# computed separately in each; only this dotted version is shared.
#
# Bump on release (the git tag + appcast still gate the actual rollout).
set(APP_VERSION "0.2.4")

# macOS deliberately trails Windows: the mac port doesn't yet have the 0.2.0 skin /
# theme engine, so it ships as 0.1.9. This is the one intentional exception to the
# "both platforms share one version" rule — the mac build (APPLE) overrides it here,
# feeding both CFBundleShortVersionString and the generated version.h. Windows
# (the .exe / windows-core CI) is unaffected and stays on 0.2.x.
if(APPLE)
  set(APP_VERSION "0.1.10")
endif()
