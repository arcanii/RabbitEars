@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Build the Windows installer with Inno Setup. Run after a normal GUI build
REM (scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON) so build\RabbitEars.exe, its
REM DLLs, and build\plugins\ exist.
setlocal EnableExtensions

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo ERROR: Inno Setup not found. Install it: winget install JRSoftware.InnoSetup
  exit /b 1
)

if not exist "%~dp0..\build\RabbitEars.exe" (
  echo ERROR: build\RabbitEars.exe not found. Run scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON first.
  exit /b 1
)

"%ISCC%" "%~dp0..\packaging\installer.iss" || exit /b 1
echo.
echo Installer written to build\installer\
