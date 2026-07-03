; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup script for RabbitEars (Win32). Build with:
;   scripts\build-installer.cmd   (after a normal GUI build)
; Produces build\installer\RabbitEars-<ver>-setup.exe.
;
; Keep MyVer in sync with APP_VERSION (CMakeLists.txt), packaging/app.manifest,
; and packaging/RabbitEars.rc on each release — see docs/RELEASING.md.

#define MyApp "RabbitEars"
#define MyVer "0.1.5"

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
OutputBaseFilename=RabbitEars-{#MyVer}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "..\build\RabbitEars.exe"; DestDir: "{app}"; Flags: ignoreversion
; Runtime DLLs (libvlc, libvlccore, WinSparkle) copied next to the exe by the build.
Source: "..\build\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; libVLC auto-discovers its plugins relative to libvlc.dll — ship the whole tree.
Source: "..\build\plugins\*"; DestDir: "{app}\plugins"; \
  Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\RabbitEars"; Filename: "{app}\RabbitEars.exe"
Name: "{group}\Uninstall RabbitEars"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\RabbitEars.exe"; Description: "Launch RabbitEars"; \
  Flags: nowait postinstall skipifsilent
