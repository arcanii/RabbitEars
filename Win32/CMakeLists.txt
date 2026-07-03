# SPDX-License-Identifier: GPL-3.0-or-later
#
# Win32/ — the Windows app: the WinHTTP/known-folder platform layer, the headless
# CLI, and the Direct2D GUI. Consumes RabbitEarsCore from ../common.

# ---------------------------------------------------------------------------
# RabbitEarsPlatformWin — the Windows platform layer the core deliberately
# excludes: WinHTTP download (Http.cpp) + the %LOCALAPPDATA% db path (Paths.cpp,
# which implements Database::defaultDbPath). Linked by the CLI and the GUI.
# The macOS peers are ../mac/platform/*.
# ---------------------------------------------------------------------------
add_library(RabbitEarsPlatformWin STATIC
  platform/Http.cpp
  platform/Paths.cpp)
target_link_libraries(RabbitEarsPlatformWin PUBLIC RabbitEarsCore shell32 ole32 winhttp)

# ---------------------------------------------------------------------------
# RabbitEarsCli — headless test/inspection tool for the core (--selftest).
# ---------------------------------------------------------------------------
add_executable(RabbitEarsCli cli/RabbitEarsCli.cpp)
target_link_libraries(RabbitEarsCli PRIVATE RabbitEarsPlatformWin)
add_test(NAME selftest COMMAND RabbitEarsCli --selftest)  # peer of mac/'s selftest

# ---------------------------------------------------------------------------
# RabbitEars — the Win32 GUI app. Gated behind RABBITEARS_BUILD_GUI because it
# provisions libVLC (a ~130 MB NuGet download at configure time). See
# cmake/LibVlc.cmake.
# ---------------------------------------------------------------------------
if(RABBITEARS_BUILD_GUI)
  enable_language(RC)  # the .rc resource compiler — Windows-only, GUI-only
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
  include(LibVlc)
  rabbitears_provision_libvlc()
  if(NOT LIBVLC_FOUND)
    message(FATAL_ERROR "RABBITEARS_BUILD_GUI is ON but libVLC could not be provisioned.")
  endif()

  # WinSparkle (auto-update) — vendored prebuilt DLL/lib/header (copied from the
  # sibling app). Same appcast/EdDSA scheme; see Win32/platform/Updater.cpp.
  set(WS_DIR "${CMAKE_SOURCE_DIR}/third_party/winsparkle")
  add_library(WinSparkle SHARED IMPORTED)
  set_target_properties(WinSparkle PROPERTIES
    IMPORTED_IMPLIB "${WS_DIR}/lib/WinSparkle.lib"
    IMPORTED_LOCATION "${WS_DIR}/bin/WinSparkle.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${WS_DIR}/include")

  add_executable(RabbitEars WIN32
    WinMain.cpp
    ui/MainWindow.cpp
    ui/ChannelGridControl.cpp
    ui/BufferMeter.cpp
    ui/MiniMeter.cpp
    ui/Dialogs.cpp
    ui/Splash.cpp
    ui/VlcPlayer.cpp
    audio/SpectrumTap.cpp
    platform/Log.cpp
    platform/Updater.cpp
    "${CMAKE_SOURCE_DIR}/packaging/RabbitEars.rc")
  # The .rc pulls in app.manifest, app.ico, and the About/splash PNGs — declare
  # them so editing any forces a .res rebuild (RC dependency scanning misses these).
  set_source_files_properties("${CMAKE_SOURCE_DIR}/packaging/RabbitEars.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/app.manifest;${CMAKE_SOURCE_DIR}/packaging/app.ico;${CMAKE_SOURCE_DIR}/art/RabbitEars3.png;${CMAKE_SOURCE_DIR}/art/RabbitEars_logo.png;${CMAKE_SOURCE_DIR}/art/BadAss_RabbitEars.png")
  target_include_directories(RabbitEars PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"        # Win32/ — for ui/*.h, audio/*.h, resource.h
    ${LIBVLC_INCLUDE_DIR}
    "${CMAKE_BINARY_DIR}/generated")     # version.h
  target_link_directories(RabbitEars PRIVATE ${LIBVLC_LIB_DIR})
  target_link_libraries(RabbitEars PRIVATE
    RabbitEarsCore RabbitEarsPlatformWin ${LIBVLC_LIBS} WinSparkle
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
