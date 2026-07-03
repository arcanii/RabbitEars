# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(RabbitEars LANGUAGES C CXX)  # RC (Win) enabled in Win32/; OBJCXX in mac/

# Unified entry point. One `cmake -S . -B build` builds the right app per OS:
#   common/  — platform-neutral engine (RabbitEarsCore), built on BOTH
#   Win32/   — the Windows app (GUI + CLI), built when WIN32
#   mac/     — the macOS app + self-test, built when APPLE
if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "RabbitEars builds on Windows (Win32/) and macOS (mac/).")
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

option(RABBITEARS_BUILD_GUI "Build the RabbitEars Windows GUI app (downloads + links libVLC)" OFF)

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
# (BUILD_NUMBER is monotonic per commit, for the Sparkle update comparison).
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AppVersion.cmake")  # sets APP_VERSION (shared by both apps)
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
configure_file("${CMAKE_SOURCE_DIR}/common/version.h.in"
               "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)

# ---------------------------------------------------------------------------
# sqlite3 — vendored public-domain amalgamation, shared by both apps. Built as a
# static lib; the /W4 wall is gated to CXX above so it never applies.
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
# Per-platform subdirectories. common/ builds the shared RabbitEarsCore on both
# OSes; Win32/ and mac/ each consume it and add their platform layer + app.
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(common)
if(WIN32)
  add_subdirectory(Win32)
endif()
if(APPLE)
  add_subdirectory(mac)
endif()
