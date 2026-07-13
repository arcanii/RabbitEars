; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup script for RabbitEars (Win32). Build with:
;   scripts\build-installer.cmd   (after a normal GUI build)
; Produces build\installer\RabbitEars-<ver>-setup.exe.
;
; Keep MyVer in sync with APP_VERSION (cmake/AppVersion.cmake), packaging/app.manifest,
; and packaging/RabbitEars.rc on each release — see docs/RELEASING.md.

#define MyApp "RabbitEars"
#define MyVer "0.2.10"

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
#if ArchAllowed != "arm64"
; libVLC plugin-cache generator (third_party/vlc-tools/README.md): shipped + run post-install on
; NATIVE x64 machines only, so libVLC loads plugins.dat instead of rescanning 323 DLLs each launch
; (~10s cold start fixed). Same install-time approach as VLC's own installer — the cache's
; path/mtime/size records then exactly match the installed files. Never run under ARM emulation
; (empty-cache danger, see the README) — hence the IsX64Native gate.
Source: "..\third_party\vlc-tools\x64\vlc-cache-gen.exe"; DestDir: "{app}"; \
  Check: IsX64Native; Flags: ignoreversion
#endif
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
; Third-party attribution + the LGPL/GPL/MIT texts of the bundled components (libVLC + its
; GPL plugins, SQLite, miniz, WinSparkle). Ships beside LICENSE.txt to satisfy the notice/
; source-availability obligations — see ..\THIRD-PARTY-NOTICES.txt.
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\RabbitEars"; Filename: "{app}\RabbitEars.exe"
Name: "{group}\Uninstall RabbitEars"; Filename: "{uninstallexe}"

[Run]
#if ArchAllowed != "arm64"
; Generate the libVLC plugin cache in the (elevated, hence writable) install context — runs on
; EVERY install/update, incl. WinSparkle silent updates, so a refreshed plugins\ tree always gets
; a fresh cache. Native-x64 machines only (see the [Files] note).
Filename: "{app}\vlc-cache-gen.exe"; Parameters: """{app}\plugins"""; \
  Check: IsX64Native; StatusMsg: "Optimizing playback engine startup..."; Flags: runhidden
#endif
Filename: "{app}\RabbitEars.exe"; Description: "Launch RabbitEars"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
; plugins.dat is GENERATED post-install (not in [Files]), so the uninstaller wouldn't remove it
; and would leave {app}\plugins behind. Delete it explicitly so the uninstall is clean.
Type: files; Name: "{app}\plugins\plugins.dat"

[Code]
{ ProcessorArchitecture reports the machine's TRUE architecture (paArm64 on Windows-on-ARM even
  though Setup itself runs as an x64 process there). IsArm64Native steers the universal installer's
  per-arch [Files] split; IsX64Native gates vlc-cache-gen (see below) to machines where running it
  is safe — under x64-EMULATION on ARM it silently writes an EMPTY cache, which libVLC would trust
  and load 0 plugins -> no playback. }
function IsArm64Native: Boolean;
begin
  Result := ProcessorArchitecture = paArm64;
end;

function IsX64Native: Boolean;
begin
  Result := ProcessorArchitecture = paX64;
end;
