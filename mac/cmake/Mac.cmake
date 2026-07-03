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

  # ---- Sparkle (framework) — auto-provisioned from the GitHub release (the mac
  #      analogue of the Windows WinSparkle NuGet flow), or a caller-supplied
  #      -DSPARKLE_FRAMEWORK=<path/Sparkle.framework>. ----
  set(_sparkle_fw "")
  if(SPARKLE_FRAMEWORK AND EXISTS "${SPARKLE_FRAMEWORK}")
    set(_sparkle_fw "${SPARKLE_FRAMEWORK}")
  else()
    set(SPARKLE_VERSION "2.6.4" CACHE STRING "Sparkle release to fetch")
    set(_sp_root "${CMAKE_BINARY_DIR}/sparkle")
    # The release tar.xz ships a plain Sparkle.framework at top level; some builds
    # ship an xcframework — accept either.
    file(GLOB _sp_found "${_sp_root}/Sparkle.framework"
                        "${_sp_root}/Sparkle.xcframework/macos*/Sparkle.framework")
    if(NOT _sp_found)
      set(_tar "${CMAKE_BINARY_DIR}/Sparkle-${SPARKLE_VERSION}.tar.xz")
      set(_url "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz")
      message(STATUS "Sparkle: downloading ${_url}")
      file(DOWNLOAD "${_url}" "${_tar}" STATUS _st TLS_VERIFY ON)
      list(GET _st 0 _code)
      if(_code EQUAL 0)
        file(MAKE_DIRECTORY "${_sp_root}")
        file(ARCHIVE_EXTRACT INPUT "${_tar}" DESTINATION "${_sp_root}")
        file(GLOB _sp_found "${_sp_root}/Sparkle.framework"
                            "${_sp_root}/Sparkle.xcframework/macos*/Sparkle.framework")
      else()
        list(GET _st 1 _msg)
        message(WARNING "Sparkle: download failed (${_msg}).")
      endif()
    endif()
    if(_sp_found)
      list(GET _sp_found 0 _sparkle_fw)
    endif()
  endif()

  if(_sparkle_fw)
    set(SPARKLE_FRAMEWORK_DIR "${_sparkle_fw}" PARENT_SCOPE)
    set(RABBITEARS_HAVE_SPARKLE TRUE PARENT_SCOPE)
    message(STATUS "Sparkle: using ${_sparkle_fw}")
  else()
    set(RABBITEARS_HAVE_SPARKLE FALSE PARENT_SCOPE)
    message(WARNING "Sparkle not provisioned — building the app WITHOUT auto-update. "
                    "Provide -DSPARKLE_FRAMEWORK=<path/Sparkle.framework>, or check network "
                    "access for the release download. See docs/MACOS_PORT.md.")
  endif()
endfunction()
