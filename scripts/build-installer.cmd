@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Build the Windows installer(s) with Inno Setup. Run after the matching GUI build(s).
REM   scripts\build-installer.cmd            x64       from build\Win32\       -> RabbitEars-<ver>-setup.exe
REM   scripts\build-installer.cmd arm64      ARM64     from build-arm64\Win32\ -> RabbitEars-<ver>-arm64-setup.exe
REM   scripts\build-installer.cmd universal  BOTH sets -> RabbitEars-<ver>-universal-setup.exe
REM                                          (bundles both; installs the native arch at install time)
setlocal EnableExtensions

REM Arch selection (default x64).
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "NEED2="
if /i "%ARCH%"=="arm64" (
  set "BUILDSUB=build-arm64\Win32"
  set "ISCCDEFS=/DSrcDir=..\build-arm64\Win32 /DOutSuffix=-arm64 /DArchAllowed=arm64"
) else if /i "%ARCH%"=="x64" (
  set "BUILDSUB=build\Win32"
  set "ISCCDEFS="
) else if /i "%ARCH%"=="universal" (
  set "BUILDSUB=build\Win32"
  set "NEED2=build-arm64\Win32"
  set "ISCCDEFS=/DUniversal"
) else (
  echo ERROR: unknown arch "%ARCH%" ^(use x64, arm64, or universal^).
  exit /b 1
)

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo ERROR: Inno Setup not found. Install it: winget install JRSoftware.InnoSetup
  exit /b 1
)

if not exist "%~dp0..\%BUILDSUB%\RabbitEars.exe" (
  echo ERROR: %BUILDSUB%\RabbitEars.exe not found. Build the x64 GUI first ^(scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON^).
  exit /b 1
)
if defined NEED2 if not exist "%~dp0..\%NEED2%\RabbitEars.exe" (
  echo ERROR: %NEED2%\RabbitEars.exe not found. Build the ARM64 GUI first ^(scripts\build-arm64.cmd^).
  exit /b 1
)

"%ISCC%" %ISCCDEFS% "%~dp0..\packaging\installer.iss" || exit /b 1
echo.
echo %ARCH% installer written to build\installer\
