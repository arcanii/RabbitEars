# SPDX-License-Identifier: GPL-3.0-or-later
#
# macOS build entry for RabbitEars — the Phase-0 spike (see docs/MACOS_PORT.md).
# This is a SEPARATE, branch-local CMake project so the repo-root CMakeLists.txt
# (owned by the Windows team) is NOT touched. It reaches up into the shared
# ../src and ../third_party sources and compiles them IN PLACE — nothing is
# copied, so there is no divergent core to reconcile.
#
#   cmake -S mac -B build-mac                       # core + self-test only
#   cmake --build build-mac
#   ctest --test-dir build-mac --output-on-failure  # runs the core self-test
#   cmake -S mac -B build-mac -DRABBITEARS_BUILD_MAC=ON   # + the .app (needs deps)
cmake_minimum_required(VERSION 3.21)
project(RabbitEarsMac LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "mac/CMakeLists.txt is the macOS build; configure it on macOS.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SHARED_SRC "${REPO_ROOT}/src")
set(MAC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
option(RABBITEARS_BUILD_MAC "Build the macOS .app bundle (needs libVLC + Sparkle)" OFF)

# ---------------------------------------------------------------------------
# Version header — reuse the SAME src/version.h.in + git-commit-count scheme as
# the Windows build, and the SAME shared APP_VERSION (cmake/AppVersion.cmake) so
# the two platforms can never drift, comparing correctly against one appcast.
# ---------------------------------------------------------------------------
include("${REPO_ROOT}/cmake/AppVersion.cmake")  # sets APP_VERSION (shared with root)
find_package(Git QUIET)
set(BUILD_NUMBER 0)
if(GIT_FOUND AND EXISTS "${REPO_ROOT}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
    WORKING_DIRECTORY "${REPO_ROOT}"
    OUTPUT_VARIABLE BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT BUILD_NUMBER MATCHES "^[0-9]+$")
  set(BUILD_NUMBER 0)
endif()
configure_file("${SHARED_SRC}/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# ---------------------------------------------------------------------------
# sqlite3 — the SAME vendored amalgamation the Windows build uses, as C.
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC "${REPO_ROOT}/third_party/sqlite/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${REPO_ROOT}/third_party/sqlite")
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1)

# ---------------------------------------------------------------------------
# RabbitEarsCoreMac — the genuinely-portable shared logic compiled IN PLACE.
# Pure C++ (no Foundation), so the self-test is a real portability proof. The
# shared headers (platform/Encoding.h, ui/DockLayout.h) now carry their own
# non-Windows branch, so the old shim include dir is gone. Paths.cpp supplies
# the macOS Database::defaultDbPath (peer of src/platform/win/Paths.cpp).
# ---------------------------------------------------------------------------
add_library(RabbitEarsCoreMac STATIC
  "${SHARED_SRC}/core/M3uParser.cpp"
  "${SHARED_SRC}/db/Database.cpp"
  "${SHARED_SRC}/ui/DockLayout.cpp"
  "${SHARED_SRC}/platform/mac/Paths.cpp")
target_include_directories(RabbitEarsCoreMac PUBLIC "${SHARED_SRC}")
target_link_libraries(RabbitEarsCoreMac PUBLIC sqlite3)

# ---------------------------------------------------------------------------
# RabbitEarsPlatformMac — the ObjC++ platform seams (need Foundation). Linked by
# the app, NOT by the self-test, keeping the self-test dependency-free.
# ---------------------------------------------------------------------------
add_library(RabbitEarsPlatformMac STATIC
  "${SHARED_SRC}/platform/mac/Http.mm"
  "${SHARED_SRC}/platform/mac/Log.mm")
target_link_libraries(RabbitEarsPlatformMac PUBLIC RabbitEarsCoreMac "-framework Foundation")

# ---------------------------------------------------------------------------
# RabbitEarsSelfTest — proves the shared core end-to-end on macOS.
# ---------------------------------------------------------------------------
add_executable(RabbitEarsSelfTest "${MAC_SRC}/tools/selftest.cpp")
target_link_libraries(RabbitEarsSelfTest PRIVATE RabbitEarsCoreMac)
enable_testing()
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
    "${MAC_SRC}/app/main.mm"
    "${MAC_SRC}/app/AppDelegate.mm"
    "${MAC_SRC}/app/VlcPlayerMac.mm"
    "${SHARED_SRC}/platform/mac/Updater.mm")
  target_include_directories(RabbitEars PRIVATE
    "${SHARED_SRC}" "${CMAKE_BINARY_DIR}/generated")
  # PlatformMac PUBLIC-links CoreMac, so linking it alone gives the app both
  # libraries + their include dirs (listing CoreMac again just warns on dupes).
  target_link_libraries(RabbitEars PRIVATE
    RabbitEarsPlatformMac
    "-framework Cocoa" "-framework AppKit")
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
  endif()
  if(RABBITEARS_HAVE_SPARKLE)
    target_compile_definitions(RabbitEars PRIVATE RABBITEARS_HAVE_SPARKLE)
    target_link_libraries(RabbitEars PRIVATE "-framework Sparkle" "-F${SPARKLE_FRAMEWORK_DIR}/..")
  endif()
endif()
