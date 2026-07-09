@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Build a NATIVE ARM64 RabbitEars (Windows-on-ARM): native ARM64 MSVC toolchain, the arm64
REM libVLC binaries (LibVlc.cmake auto-selects build/arm64), plus the arm64 WinSparkle slice
REM under third_party/winsparkle/lib/arm64 + bin/arm64, so auto-update is compiled in exactly
REM like x64 -- RABBITEARS_UPDATER now defaults ON for both arches. Native cold-start ~4x the
REM emulated x64 build. Output lands in build-arm64\ so it never touches the x64 build\ tree.
REM   scripts\build-arm64.cmd            (theme engine off - simplest)
REM   scripts\build-arm64.cmd -DRABBITEARS_THEME_ENGINE=ON
setlocal EnableExtensions

set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsarm64.bat" (
  echo ERROR: ARM64 native toolchain not found at "%VSROOT%".
  echo Install the "MSVC ARM64/ARM64EC build tools" VS component.
  exit /b 1
)

REM VS-bundled CMake/Ninja + the VS Installer (vswhere) on PATH, then the ARM64 MSVC env.
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsarm64.bat" >nul || exit /b 1

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build-arm64"

cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
      -DRABBITEARS_BUILD_GUI=ON %* || exit /b 1
cmake --build "%BUILD%" || exit /b 1
exit /b %ERRORLEVEL%
