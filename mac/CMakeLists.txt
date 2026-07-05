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
# MeterModel.cpp is pure C++ (the mac-local meter model, rabbitears::mac) — compiled
# in here too so the codec round-trip is exercised headlessly (it is NOT in
# RabbitEarsCore yet; it lives in mac/ until promotion to common/ — see MeterModel.h).
add_executable(RabbitEarsSelfTest src/tools/selftest.cpp src/app/MeterModel.cpp)
target_include_directories(RabbitEarsSelfTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/app")
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

  # App icon: the .icns (built from art/macos_icon.png by scripts/make-icns.py) is
  # copied into Contents/Resources and named in Info.plist via CFBundleIconFile.
  set(RE_MAC_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/RabbitEars.icns")
  set_source_files_properties("${RE_MAC_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

  add_executable(RabbitEars MACOSX_BUNDLE
    src/app/main.mm
    src/app/AppDelegate.mm
    src/app/MainWindowController.mm
    src/app/VlcPlayerMac.mm
    src/app/StatMeterView.mm
    src/app/SpectrumTap.mm
    src/app/SpectrumMeterView.mm
    src/app/MetersDialog.mm
    src/app/MeterModel.cpp
    platform/Log.mm
    platform/Updater.mm
    "${RE_MAC_ICNS}")

  # These use ARC (weak-self timers; CoreFoundation bridging + a block-owning C++
  # struct for the Core Audio spectrum tap). The rest of the app is MRC-style.
  set_source_files_properties(
    src/app/StatMeterView.mm src/app/SpectrumTap.mm src/app/SpectrumMeterView.mm
    src/app/MetersDialog.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

  target_include_directories(RabbitEars PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"   # AppDelegate.h, VlcPlayerMac.h
    "${CMAKE_BINARY_DIR}")                  # "generated/version.h"
  target_link_libraries(RabbitEars PRIVATE
    RabbitEarsPlatformMac "-framework Cocoa" "-framework AppKit"
    "-framework CoreAudio" "-framework Accelerate")
  set_target_properties(RabbitEars PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "RabbitEars"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.rabbitears.RabbitEars"
    MACOSX_BUNDLE_BUNDLE_VERSION "${BUILD_NUMBER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "RabbitEars.icns")

  if(RABBITEARS_HAVE_LIBVLC)
    target_compile_definitions(RabbitEars PRIVATE RABBITEARS_HAVE_LIBVLC)
    target_include_directories(RabbitEars PRIVATE "${LIBVLC_MAC_INCLUDE_DIR}")
    target_link_libraries(RabbitEars PRIVATE "${LIBVLC_MAC_LIB}")
    # Runtime: rpath so @rpath/libvlc.dylib resolves, + point libVLC at its plugins.
    set_property(TARGET RabbitEars APPEND PROPERTY BUILD_RPATH "${LIBVLC_MAC_LIB_DIR}")
    set_property(TARGET RabbitEars APPEND PROPERTY INSTALL_RPATH "${LIBVLC_MAC_LIB_DIR}")
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
    get_filename_component(_sparkle_slice "${SPARKLE_FRAMEWORK_DIR}" DIRECTORY)  # dir holding Sparkle.framework
    target_compile_options(RabbitEars PRIVATE "-F${_sparkle_slice}")  # so #import <Sparkle/Sparkle.h> resolves
    target_link_libraries(RabbitEars PRIVATE "-framework Sparkle" "-F${_sparkle_slice}")
    # Embed Sparkle.framework in the bundle (cp -R preserves its symlinks + XPC
    # services) + an rpath so @rpath/Sparkle.framework resolves at runtime.
    set_property(TARGET RabbitEars APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    set_property(TARGET RabbitEars APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")
    add_custom_command(TARGET RabbitEars POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:RabbitEars>/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_BUNDLE_CONTENT_DIR:RabbitEars>/Frameworks/Sparkle.framework"
      COMMAND cp -R "${SPARKLE_FRAMEWORK_DIR}" "$<TARGET_BUNDLE_CONTENT_DIR:RabbitEars>/Frameworks/"
      COMMENT "Embedding Sparkle.framework into the bundle")
  endif()
endif()
