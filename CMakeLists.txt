# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(RabbitEars LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "RabbitEars is Win32-only for now (macOS/Linux targets are on the roadmap).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Statically link the MSVC CRT (/MT) so the exe does not depend on the VC++
# redistributable DLLs (MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1) being present —
# the most common "missing DLL" failure for a freshly-built native exe, and the
# right choice for a portable/distributed app. Safe with libVLC: only C strings
# and handles cross the DLL boundary, never CRT-owned objects. (Requires CMP0091,
# default-NEW at our cmake_minimum_required.)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # RelWithDebInfo, not Debug: the MSVC Debug CRT heap serializes all allocations
  # behind one process-wide lock, which stalled the sibling apps' UI thread during
  # heavy background work. Release CRT + PDBs keeps debugging usable. (build.cmd
  # passes this explicitly; this is just the bare-cmake fallback.)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(RABBITEARS_BUILD_GUI "Build the RabbitEars GUI app (downloads + links libVLC)" OFF)

if(MSVC)
  # /W4 /permissive- /EHsc are CXX-only so they never hit the C sqlite amalgamation;
  # /utf-8 applies to both (our UTF-8 string literals + the vendored headers).
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    /utf-8)
  add_compile_definitions(
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006
    _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Version header — APP_VERSION + git commit count -> build/generated/version.h
# (BUILD_NUMBER is monotonic per commit, for WinSparkle update comparison).
# ---------------------------------------------------------------------------
set(APP_VERSION "0.1.6")
find_package(Git QUIET)
set(BUILD_NUMBER 0)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT BUILD_NUMBER MATCHES "^[0-9]+$")
  set(BUILD_NUMBER 0)
endif()
configure_file("${CMAKE_SOURCE_DIR}/src/version.h.in"
               "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# ---------------------------------------------------------------------------
# sqlite3 — vendored public-domain amalgamation (copied from the sibling app).
# Built as a static lib; the /W4 wall is gated to CXX above so it never applies.
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
  _CRT_SECURE_NO_WARNINGS
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DQS=0                    # reject double-quoted string literals (safety)
  SQLITE_DEFAULT_FOREIGN_KEYS=1)
if(MSVC)
  target_compile_options(sqlite3 PRIVATE /w)  # don't hold third-party C to our bar
endif()

# ---------------------------------------------------------------------------
# RabbitEarsCore — engine-only: M3U parsing + SQLite persistence. No UI, no
# libVLC, so it (and the CLI) build and test without downloading anything.
# ---------------------------------------------------------------------------
add_library(RabbitEarsCore STATIC
  src/core/M3uParser.cpp
  src/core/Http.cpp
  src/db/Database.cpp
  src/ui/DockLayout.cpp)  # pure layout-tree logic (no HWNDs) — unit-tested by the CLI
target_include_directories(RabbitEarsCore PUBLIC src)
target_link_libraries(RabbitEarsCore PUBLIC sqlite3 shell32 ole32 winhttp)

# ---------------------------------------------------------------------------
# RabbitEarsCli — headless test/inspection tool for the core (--selftest).
# ---------------------------------------------------------------------------
add_executable(RabbitEarsCli src/cli/RabbitEarsCli.cpp)
target_link_libraries(RabbitEarsCli PRIVATE RabbitEarsCore)

# ---------------------------------------------------------------------------
# RabbitEars — the Win32 GUI app. Gated behind RABBITEARS_BUILD_GUI because it
# provisions libVLC (a ~130 MB NuGet download at configure time) and links the
# UI. Enabled once the UI sources land (next layer). See cmake/LibVlc.cmake.
# ---------------------------------------------------------------------------
if(RABBITEARS_BUILD_GUI)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  include(LibVlc)
  rabbitears_provision_libvlc()
  if(NOT LIBVLC_FOUND)
    message(FATAL_ERROR "RABBITEARS_BUILD_GUI is ON but libVLC could not be provisioned.")
  endif()

  # WinSparkle (auto-update) — vendored prebuilt DLL/lib/header (copied from the
  # sibling app). Same appcast/EdDSA scheme; see src/platform/Updater.cpp.
  set(WS_DIR "${CMAKE_SOURCE_DIR}/third_party/winsparkle")
  add_library(WinSparkle SHARED IMPORTED)
  set_target_properties(WinSparkle PROPERTIES
    IMPORTED_IMPLIB "${WS_DIR}/lib/WinSparkle.lib"
    IMPORTED_LOCATION "${WS_DIR}/bin/WinSparkle.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${WS_DIR}/include")

  add_executable(RabbitEars WIN32
    src/WinMain.cpp
    src/ui/MainWindow.cpp
    src/ui/ChannelGridControl.cpp
    src/ui/BufferMeter.cpp
    src/ui/MiniMeter.cpp
    src/ui/Dialogs.cpp
    src/ui/Splash.cpp
    src/ui/VlcPlayer.cpp
    src/audio/SpectrumTap.cpp
    src/platform/Log.cpp
    src/platform/Updater.cpp
    packaging/RabbitEars.rc)
  # The .rc pulls in app.manifest, app.ico, and the About PNG — declare them so
  # editing any forces a .res rebuild (RC dependency scanning misses these).
  set_source_files_properties(packaging/RabbitEars.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/packaging/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/art/RabbitEars3.png;${CMAKE_CURRENT_SOURCE_DIR}/art/RabbitEars_logo.png")
  target_include_directories(RabbitEars PRIVATE src ${LIBVLC_INCLUDE_DIR}
    "${CMAKE_BINARY_DIR}/generated")
  target_link_directories(RabbitEars PRIVATE ${LIBVLC_LIB_DIR})
  target_link_libraries(RabbitEars PRIVATE
    RabbitEarsCore ${LIBVLC_LIBS} WinSparkle
    comctl32 comdlg32 dwmapi uxtheme gdi32 gdiplus shell32 user32 kernel32 ole32 version
    d2d1 dwrite windowscodecs winhttp mmdevapi)
  if(MSVC)
    target_link_options(RabbitEars PRIVATE /MANIFEST:NO)  # manifest is embedded via the .rc
  endif()

  # Deploy the libVLC runtime next to the exe: the two DLLs + the whole plugins/
  # tree (libVLC auto-discovers plugins relative to libvlc.dll).
  foreach(_dll ${LIBVLC_DLLS})
    add_custom_command(TARGET RabbitEars POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${LIBVLC_LIB_DIR}/${_dll}" "$<TARGET_FILE_DIR:RabbitEars>"
      COMMENT "Copying ${_dll} next to RabbitEars.exe")
  endforeach()
  add_custom_command(TARGET RabbitEars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LIBVLC_PLUGINS_DIR}" "$<TARGET_FILE_DIR:RabbitEars>/plugins"
    COMMENT "Copying libVLC plugins/ next to RabbitEars.exe")
  add_custom_command(TARGET RabbitEars POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WS_DIR}/bin/WinSparkle.dll" "$<TARGET_FILE_DIR:RabbitEars>"
    COMMENT "Copying WinSparkle.dll next to RabbitEars.exe")
endif()
