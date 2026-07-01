@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Configure and build RabbitEars with the VS 2026 toolchain (CMake + Ninja),
REM matching the sibling apps' build story. Pass extra CMake args through, e.g.:
REM    scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON
setlocal EnableExtensions

set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: Visual Studio 2026 not found at "%VSROOT%".
  exit /b 1
)

REM VS-bundled CMake/Ninja + the VS Installer (vswhere) on PATH, then MSVC env.
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build"

REM RelWithDebInfo: release CRT (no debug-heap global lock that stalls the UI
REM thread during background work) while still emitting PDBs for debugging.
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo %* || exit /b 1
cmake --build "%BUILD%" || exit /b 1
exit /b %ERRORLEVEL%
