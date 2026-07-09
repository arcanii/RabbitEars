; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup script for RabbitEars (Win32). Build with:
;   scripts\build-installer.cmd   (after a normal GUI build)
; Produces build\installer\RabbitEars-<ver>-setup.exe.
;
; Keep MyVer in sync with APP_VERSION (CMakeLists.txt), packaging/app.manifest,
; and packaging/RabbitEars.rc on each release — see docs/RELEASING.md.

#define MyApp "RabbitEars"
#define MyVer "0.2.5"

; Build-variant selection (see docs/RELEASING.md + scripts\build-installer.cmd):
;   (defaults)   -> x64 installer from build\Win32\  (byte-identical to the original script)
;   arm64 (/D..) -> native ARM64 installer from build-arm64\Win32\ (arm64-only architecture)
;   /DUniversal  -> ONE installer bundling BOTH binary sets that installs the NATIVE arch at
;                   install time (Inno Check: ProcessorArchitecture). The installed layout is
;                   identical to the per-arch installers, so auto-update flows through the same
;                   appcast.xml / appcast-arm64.xml unchanged — no launcher, no extra feed.
#ifndef SrcDir
  #define SrcDir "..\build\Win32"
#endif
#ifndef ArchAllowed
  #define ArchAllowed "x64compatible"
#endif
#ifdef Universal
  #define OutSuffix "-universal"
  #define ArchList "x64compatible or arm64"
#else
  #ifndef OutSuffix
    #define OutSuffix ""
  #endif
  #define ArchList ArchAllowed
#endif

[Setup]
AppId={{E5C26129-79DE-4A86-8C69-0AF1B95B2130}}
AppName={#MyApp}
AppVersion={#MyVer}
AppPublisher=RabbitEars
DefaultDirName={autopf}\{#MyApp}
DefaultGroupName={#MyApp}
DisableProgramGroupPage=yes
SetupIconFile=app.ico
UninstallDisplayIcon={app}\RabbitEars.exe
OutputDir=..\build\installer
OutputBaseFilename=RabbitEars-{#MyVer}{#OutSuffix}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed={#ArchList}
ArchitecturesInstallIn64BitMode={#ArchList}
WizardStyle=modern
; Auto-update robustness: use Restart Manager to close a running instance before
; overwriting files, then relaunch it. AppMutex matches the app's single-instance mutex
; (src/ui/MainWindow.cpp runApp) so Inno can detect a still-running copy.
AppMutex=RabbitEars.SingleInstance
CloseApplications=yes
RestartApplications=yes

[Files]
#ifdef Universal
; Universal: bundle BOTH binary sets; the Check installs only the native arch's files, so the
; on-disk result matches a per-arch install exactly (one RabbitEars.exe + its DLLs + plugins).
Source: "..\build\Win32\RabbitEars.exe"; DestDir: "{app}"; Check: not IsArm64Native; Flags: ignoreversion
Source: "..\build\Win32\*.dll"; DestDir: "{app}"; Check: not IsArm64Native; Flags: ignoreversion
Source: "..\build\Win32\plugins\*"; DestDir: "{app}\plugins"; Check: not IsArm64Native; \
  Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build-arm64\Win32\RabbitEars.exe"; DestDir: "{app}"; Check: IsArm64Native; Flags: ignoreversion
Source: "..\build-arm64\Win32\*.dll"; DestDir: "{app}"; Check: IsArm64Native; Flags: ignoreversion
Source: "..\build-arm64\Win32\plugins\*"; DestDir: "{app}\plugins"; Check: IsArm64Native; \
  Flags: ignoreversion recursesubdirs createallsubdirs
#else
Source: "{#SrcDir}\RabbitEars.exe"; DestDir: "{app}"; Flags: ignoreversion
; Runtime DLLs (libvlc, libvlccore, WinSparkle) copied next to the exe by the build.
Source: "{#SrcDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; libVLC auto-discovers its plugins relative to libvlc.dll — ship the whole tree.
Source: "{#SrcDir}\plugins\*"; DestDir: "{app}\plugins"; \
  Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\RabbitEars"; Filename: "{app}\RabbitEars.exe"
Name: "{group}\Uninstall RabbitEars"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\RabbitEars.exe"; Description: "Launch RabbitEars"; \
  Flags: nowait postinstall skipifsilent

#ifdef Universal
[Code]
{ Universal installer: lay down the binaries matching the machine's NATIVE CPU. ProcessorArchitecture
  reports the true architecture (paArm64 on Windows-on-ARM, even though Setup itself runs as an x64
  process there), so on ARM we install the native ARM64 build and on x64 the x64 build — never both. }
function IsArm64Native: Boolean;
begin
  Result := ProcessorArchitecture = paArm64;
end;
#endif
