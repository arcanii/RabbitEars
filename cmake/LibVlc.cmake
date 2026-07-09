# SPDX-License-Identifier: GPL-3.0-or-later
#
# LibVlc.cmake — self-contained provisioning of the VideoLAN.LibVLC.Windows NuGet
# package from a plain CMake + Ninja build (NO Visual Studio project, NO NuGet
# restore). Modeled 1:1 on the sibling app's cmake/WindowsAppSdk.cmake: prefer the
# machine NuGet cache, else download the .nupkg (a zip) into the build tree and
# extract it, then expose include/lib/dll/plugins paths in the caller's scope.
#
# Usage (from the top-level CMakeLists.txt):
#     include(LibVlc)
#     rabbitears_provision_libvlc()   # sets LIBVLC_* in the caller's scope
#
# On success it sets, in the CALLER's scope:
#   LIBVLC_FOUND         TRUE if the package was located/extracted.
#   LIBVLC_INCLUDE_DIR   dir containing vlc/vlc.h (so #include <vlc/vlc.h> works).
#   LIBVLC_LIB_DIR       dir containing libvlc.lib + the runtime DLLs + plugins/.
#   LIBVLC_LIBS          import libs to link (libvlc.lib — libvlccore is not needed
#                        unless you call libvlccore/module APIs directly).
#   LIBVLC_DLLS          runtime DLL filenames to copy next to the exe.
#   LIBVLC_PLUGINS_DIR   the plugins/ tree to copy next to the exe.
#
# libVLC 3.0.x is LGPLv2.1: dynamic-linking + shipping the unmodified DLLs/plugins
# keeps RabbitEars proprietary. Ship the LGPL license text + attribution.

# Pin the exact version (wraps libVLC 3.0.23, current 3.0.x stable). Bump deliberately.
set(LIBVLC_NUGET_VERSION "3.0.23.1" CACHE STRING "VideoLAN.LibVLC.Windows NuGet package version")

function(rabbitears_provision_libvlc)
  set(LIBVLC_FOUND FALSE PARENT_SCOPE)
  set(_pkg_id "videolan.libvlc.windows")
  # Match the libVLC binaries to the compiler TARGET arch (the NuGet ships build/x64, build/x86 AND
  # build/arm64). Use CMAKE_CXX_COMPILER_ARCHITECTURE_ID — the MSVC TARGET ("x64"/"ARM64"/"X86") —
  # NOT CMAKE_SYSTEM_PROCESSOR, which is the HOST: on a Windows-on-ARM box the host is ARM64 even
  # for an x64 (emulated) build, so keying on it would wrongly hand the x64 link the arm64 lib.
  if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(_rel "build/arm64")
  else()
    set(_rel "build/x64")   # x64 (default) — also the x86 fallback; ARM64 handled above
  endif()

  # -------------------------------------------------------------------------
  # 1. Locate the package: machine NuGet cache first, else download + extract.
  # -------------------------------------------------------------------------
  set(_pkg_root "")
  set(_cands "")
  if(DEFINED ENV{NUGET_PACKAGES})
    list(APPEND _cands "$ENV{NUGET_PACKAGES}/${_pkg_id}/${LIBVLC_NUGET_VERSION}")
  endif()
  if(DEFINED ENV{USERPROFILE})
    list(APPEND _cands "$ENV{USERPROFILE}/.nuget/packages/${_pkg_id}/${LIBVLC_NUGET_VERSION}")
  endif()
  foreach(_c IN LISTS _cands)
    if(EXISTS "${_c}/${_rel}/include/vlc/vlc.h")
      set(_pkg_root "${_c}")
      message(STATUS "libVLC: using cached NuGet package at ${_pkg_root}")
      break()
    endif()
  endforeach()

  if(_pkg_root STREQUAL "")
    set(_dl "${CMAKE_BINARY_DIR}/libvlc_pkg")
    if(NOT EXISTS "${_dl}/${_rel}/include/vlc/vlc.h")
      set(_nupkg "${CMAKE_BINARY_DIR}/${_pkg_id}.${LIBVLC_NUGET_VERSION}.nupkg")
      set(_url "https://api.nuget.org/v3-flatcontainer/${_pkg_id}/${LIBVLC_NUGET_VERSION}/${_pkg_id}.${LIBVLC_NUGET_VERSION}.nupkg")
      message(STATUS "libVLC: NuGet cache miss; downloading ${_url}")
      file(DOWNLOAD "${_url}" "${_nupkg}" STATUS _st SHOW_PROGRESS TLS_VERIFY ON)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        message(WARNING "libVLC: download failed (${_msg}).")
        return()
      endif()
      file(MAKE_DIRECTORY "${_dl}")
      file(ARCHIVE_EXTRACT INPUT "${_nupkg}" DESTINATION "${_dl}")
    endif()
    if(EXISTS "${_dl}/${_rel}/include/vlc/vlc.h")
      set(_pkg_root "${_dl}")
    else()
      message(WARNING "libVLC: package extraction incomplete under ${_dl}.")
      return()
    endif()
  endif()

  # -------------------------------------------------------------------------
  # 2. Resolve and validate include/lib/dll/plugins.
  # -------------------------------------------------------------------------
  set(_root "${_pkg_root}/${_rel}")
  if(NOT EXISTS "${_root}/libvlc.lib" OR NOT EXISTS "${_root}/libvlc.dll"
     OR NOT EXISTS "${_root}/libvlccore.dll" OR NOT EXISTS "${_root}/plugins")
    message(WARNING "libVLC: expected files missing under ${_root}.")
    return()
  endif()

  set(LIBVLC_FOUND       TRUE                        PARENT_SCOPE)
  set(LIBVLC_INCLUDE_DIR "${_root}/include"          PARENT_SCOPE)
  set(LIBVLC_LIB_DIR     "${_root}"                  PARENT_SCOPE)
  set(LIBVLC_LIBS        "${_root}/libvlc.lib"       PARENT_SCOPE)
  set(LIBVLC_DLLS        "libvlc.dll;libvlccore.dll" PARENT_SCOPE)
  set(LIBVLC_PLUGINS_DIR "${_root}/plugins"          PARENT_SCOPE)
  message(STATUS "libVLC: enabled (VideoLAN.LibVLC.Windows ${LIBVLC_NUGET_VERSION}) at ${_root}")
endfunction()
