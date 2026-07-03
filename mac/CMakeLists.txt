# SPDX-License-Identifier: GPL-3.0-or-later
#
# mac/ — the macOS app + shared-core self-test. A subdirectory of the unified
# root build (cmake -S . -B build on macOS): it consumes RabbitEarsCore from
# ../common and sqlite3 + the generated version.h from the root. Platform impls
# live in mac/platform/* (peers of ../Win32/platform/*).
enable_language(OBJCXX)  # root project() is C CXX; the .mm sources need OBJCXX
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

option(RABBITEARS_BUILD_MAC "Build the macOS .app bundle (needs libVLC + Sparkle)" OFF)

# ---------------------------------------------------------------------------
# RabbitEarsPlatformMac — the macOS platform layer, peer of Win32's
# RabbitEarsPlatformWin: NSURLSession HTTP (Http.mm) + the Application Support db
# path (Paths.cpp, implements Database::defaultDbPath). Linked by the self-test
# and the app. Paths.cpp is pure C++; Http.mm needs Foundation.
# ---------------------------------------------------------------------------
add_library(RabbitEarsPlatformMac STATIC
  platform/Http.mm
  platform/Paths.cpp)
target_link_libraries(RabbitEarsPlatformMac PUBLIC RabbitEarsCore "-framework Foundation")

# ---------------------------------------------------------------------------
# RabbitEarsSelfTest — proves the shared core end-to-end on macOS (peer of the
# Windows CLI --selftest).
# ---------------------------------------------------------------------------
add_executable(RabbitEarsSelfTest src/tools/selftest.cpp)
target_link_libraries(RabbitEarsSelfTest PRIVATE RabbitEarsPlatformMac)
add_test(NAME selftest COMMAND RabbitEarsSelfTest)

# ---------------------------------------------------------------------------
# RabbitEars.app — the Cocoa GUI (gated; needs libVLC + Sparkle). Playback and
# auto-update are compiled in only when their deps are provisioned.
# ---------------------------------------------------------------------------
if(RABBITEARS_BUILD_MAC)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  include(Mac)
  rabbitears_provision_mac_deps()

  add_executable(RabbitEars MACOSX_BUNDLE
    src/app/main.mm
    src/app/AppDelegate.mm
    src/app/MainWindowController.mm
    src/app/VlcPlayerMac.mm
    platform/Log.mm
    platform/Updater.mm)
  target_include_directories(RabbitEars PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"   # AppDelegate.h, VlcPlayerMac.h
    "${CMAKE_BINARY_DIR}")                  # "generated/version.h"
  target_link_libraries(RabbitEars PRIVATE
    RabbitEarsPlatformMac "-framework Cocoa" "-framework AppKit")
  set_target_properties(RabbitEars PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "RabbitEars"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.rabbitears.RabbitEars"
    MACOSX_BUNDLE_BUNDLE_VERSION "${BUILD_NUMBER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}")

  if(RABBITEARS_HAVE_LIBVLC)
    target_compile_definitions(RabbitEars PRIVATE RABBITEARS_HAVE_LIBVLC)
    target_include_directories(RabbitEars PRIVATE "${LIBVLC_MAC_INCLUDE_DIR}")
    target_link_libraries(RabbitEars PRIVATE "${LIBVLC_MAC_LIB}")
    # Runtime: rpath so @rpath/libvlc.dylib resolves, + point libVLC at its plugins.
    set_target_properties(RabbitEars PROPERTIES
      BUILD_RPATH "${LIBVLC_MAC_LIB_DIR}" INSTALL_RPATH "${LIBVLC_MAC_LIB_DIR}")
    if(LIBVLC_MAC_PLUGIN_DIR)
      target_compile_definitions(RabbitEars PRIVATE RABBITEARS_VLC_PLUGIN_PATH="${LIBVLC_MAC_PLUGIN_DIR}")
    endif()

    # RabbitEarsPlayProbe — headless libVLC smoke test (plugins load + can open a
    # stream with no video/audio device). Manual: `RabbitEarsPlayProbe [url] [secs]`
    # (needs network, so it is NOT a ctest).
    add_executable(RabbitEarsPlayProbe src/tools/playprobe.mm)
    target_include_directories(RabbitEarsPlayProbe PRIVATE "${LIBVLC_MAC_INCLUDE_DIR}")
    target_link_libraries(RabbitEarsPlayProbe PRIVATE "${LIBVLC_MAC_LIB}" "-framework Foundation")
    set_target_properties(RabbitEarsPlayProbe PROPERTIES BUILD_RPATH "${LIBVLC_MAC_LIB_DIR}")
    if(LIBVLC_MAC_PLUGIN_DIR)
      target_compile_definitions(RabbitEarsPlayProbe PRIVATE RABBITEARS_VLC_PLUGIN_PATH="${LIBVLC_MAC_PLUGIN_DIR}")
    endif()
  endif()
  if(RABBITEARS_HAVE_SPARKLE)
    target_compile_definitions(RabbitEars PRIVATE RABBITEARS_HAVE_SPARKLE)
    target_link_libraries(RabbitEars PRIVATE "-framework Sparkle" "-F${SPARKLE_FRAMEWORK_DIR}/..")
  endif()
endif()
