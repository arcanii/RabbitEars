# SPDX-License-Identifier: GPL-3.0-or-later
#
# Marketing version, included by BOTH build systems (root CMakeLists.txt for Windows,
# mac/CMakeLists.txt for macOS). The per-commit BUILD_NUMBER (git rev-list count) is
# computed separately in each; only this dotted version is shared.
#
# Bump on release (the git tag + appcast still gate the actual rollout). THIS is the single
# source of truth for the Windows version: the .exe VERSIONINFO (packaging/RabbitEars.rc via the
# generated version.h), the app manifest (generated from packaging/app.manifest.in), and the Inno
# installer (generated packaging/version.iss) all derive from it — nothing else to hand-edit.
set(APP_VERSION "0.2.11")

# macOS carries its own decoupled version: the mac port still lacks the Windows theme
# engine, so the platforms are NOT lockstep on patch numbers. This is the one intentional
# exception to the "both platforms share one version" rule — the mac build (APPLE)
# overrides it here, feeding both CFBundleShortVersionString and the generated version.h.
# Windows (the .exe / windows-core CI) is unaffected and stays on its own 0.2.x line.
#
# mac 0.2.10 reaches Windows 0.2.11 parity on the shared feature set: 0.2.0 landed multi-view + the
# TV Guide (EPG); 0.2.7 added the 0.2.6/0.2.7 set (favourites I/O, PiP resize/persist, saved layouts,
# per-pane recording, the recording scheduler + EPG series rules); 0.2.8 added localization — English
# + 日本語; 0.2.9 added the recording-rule editor, series-rule episode dedup (schema v6), Traditional
# Chinese (zh-Hant + zh-HK) in the selector, and the GPL-3.0 notices; 0.2.10 makes Settings ▸ Language
# apply LIVE (no restart), the mac peer of Windows 0.2.11. (Windows 0.2.10 was a Win32-only
# Chinese-selection hotfix, N/A to mac.) 0.2.11 is an i18n-polish release: an AI-assisted,
# adversarially-verified CJK translation-quality pass (36 verified JA/zh-Hant/zh-HK consistency
# fixes) plus a dead-catalog-id prune (6 ids). 0.2.12 ships four Win32-gap parity features
# (resume-last-channel, right-click video menu + fullscreen screen-saver suspend, Categories
# include-filter, hide-unavailable-channels — PRs #36-#39, all on-device GUI-verified). (mac
# history: 0.1.7 → 0.1.10 → 0.2.0 → 0.2.7 → 0.2.8 → 0.2.9 → 0.2.10 → 0.2.11 → 0.2.12.)
if(APPLE)
  set(APP_VERSION "0.2.12")
endif()
