# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mac.cmake — best-effort provisioning of the two external mac deps that the
# .app (but NOT the shared core or the self-test) needs: libVLC and Sparkle.
# Unlike the Windows libVLC NuGet flow (cmake/LibVlc.cmake), there is no single
# canonical mac package, so this searches a few well-known locations and a
# caller-supplied prefix. If a dep is missing it is skipped (not fatal): the app
# still builds — playback / auto-update are compiled out via the
# RABBITEARS_HAVE_LIBVLC / RABBITEARS_HAVE_SPARKLE defines.
#
# Override search with -DLIBVLC_MAC_PREFIX=<dir> (contains include/vlc/vlc.h and
# lib/libvlc.dylib) and -DSPARKLE_FRAMEWORK=<path to Sparkle.framework>.
#
# NOTE: a stock VLC.app often ships include/vlc/vlc.h + lib/libvlc.dylib, so the
# app may link real libVLC on a dev machine. Running it then also needs an rpath
# to that lib + the plugins dir — that runtime packaging is Phase-1 work.

function(rabbitears_provision_mac_deps)
  # ---- libVLC (headers + dylibs) ----
  set(_vlc_prefixes
      "${LIBVLC_MAC_PREFIX}"
      "/opt/homebrew/opt/libvlc"          # if a formula/SDK is installed
      "/usr/local/opt/libvlc"
      "/Applications/VLC.app/Contents/MacOS")  # stock app often ships headers+dylib
  set(_have_libvlc FALSE)
  foreach(_p IN LISTS _vlc_prefixes)
    if(_p AND EXISTS "${_p}/include/vlc/vlc.h" AND EXISTS "${_p}/lib/libvlc.dylib")
      set(LIBVLC_MAC_INCLUDE_DIR "${_p}/include" PARENT_SCOPE)
      set(LIBVLC_MAC_LIB "${_p}/lib/libvlc.dylib" PARENT_SCOPE)
      set(LIBVLC_MAC_LIB_DIR "${_p}/lib" PARENT_SCOPE)     # rpath so @rpath/libvlc.dylib resolves at runtime
      # plugins tree: usually <prefix>/plugins (VLC.app) — fall back to lib/vlc/plugins.
      if(EXISTS "${_p}/plugins")
        set(LIBVLC_MAC_PLUGIN_DIR "${_p}/plugins" PARENT_SCOPE)
      elseif(EXISTS "${_p}/lib/vlc/plugins")
        set(LIBVLC_MAC_PLUGIN_DIR "${_p}/lib/vlc/plugins" PARENT_SCOPE)
      endif()
      set(_have_libvlc TRUE)
      message(STATUS "libVLC(mac): using ${_p}")
      break()
    endif()
  endforeach()
  set(RABBITEARS_HAVE_LIBVLC ${_have_libvlc} PARENT_SCOPE)
  if(NOT _have_libvlc)
    message(WARNING "libVLC(mac) not found — building the app WITHOUT playback. "
                    "Set -DLIBVLC_MAC_PREFIX=<dir with include/vlc/vlc.h + lib/libvlc.dylib>. "
                    "See docs/MACOS_PORT.md.")
  endif()

  # ---- Sparkle (framework) ----
  set(_have_sparkle FALSE)
  if(SPARKLE_FRAMEWORK AND EXISTS "${SPARKLE_FRAMEWORK}")
    set(SPARKLE_FRAMEWORK_DIR "${SPARKLE_FRAMEWORK}" PARENT_SCOPE)
    set(_have_sparkle TRUE)
    message(STATUS "Sparkle: using ${SPARKLE_FRAMEWORK}")
  endif()
  set(RABBITEARS_HAVE_SPARKLE ${_have_sparkle} PARENT_SCOPE)
  if(NOT _have_sparkle)
    message(WARNING "Sparkle.framework not found — building the app WITHOUT auto-update. "
                    "Set -DSPARKLE_FRAMEWORK=<path/Sparkle.framework>. See docs/MACOS_PORT.md.")
  endif()
endfunction()
