# Releasing RabbitEars (Win32)

Auto-update uses **WinSparkle** reading an **EdDSA-signed appcast hosted on
GitHub**, mirroring the sibling apps (SQLTerminal-Win32 / ManorLords-SGE) and the
macOS Sparkle setup. RabbitEars **shares the family Ed25519 key pair**: the public
key in `src/platform/Updater.cpp` (`win_sparkle_set_eddsa_public_key`) is the same
string as the macOS `SUPublicEDKey`, so **the same private key signs Windows
packages** and signing is done **on macOS** with Sparkle's `sign_update`.
(To isolate RabbitEars, generate a dedicated key pair and paste its public key
into `Updater.cpp` instead — everything else is unchanged.)

- Feed: `https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast.xml`
- Installers: GitHub Release assets.

The running app reports its version as `APP_VERSION.BUILD` (e.g. `0.1.1.14`, from
`RE_VERSION_FULL`); WinSparkle offers an update when the appcast's
`sparkle:version` is higher.

## Distribution note (portable vs installer)
0.1.0 shipped as a **portable zip**. WinSparkle applies updates by running a
downloaded **installer**, so **only the installed build auto-updates** — ship the
Inno Setup installer below as the update-capable artifact (you can still attach the
portable zip for people who prefer it; those users update manually).

## One-time setup
- Have the **Ed25519 private key** used for the family's macOS Sparkle releases.
- A signing tool that emits a base64 Ed25519 signature:
  - macOS: Sparkle's `sign_update` (already used for the Mac apps), or
  - Windows: `sign_update.exe` from the WinSparkle release zip
    (<https://github.com/vslavik/winsparkle/releases>).
- **Inno Setup 6** for the installer (`winget install JRSoftware.InnoSetup`).
- (Optional) an Authenticode code-signing cert — see *Not yet covered*.

## Per release
1. **Bump the version** if the marketing version changes, in all four places:
   `APP_VERSION` in `CMakeLists.txt` (About box + update checks), `MyVer` in
   `packaging/installer.iss` (installer name/version), the `FILEVERSION`/
   `PRODUCTVERSION` + `FileVersion`/`ProductVersion` strings in
   `packaging/RabbitEars.rc`, and the `assemblyIdentity version` in
   `packaging/app.manifest`. The build number (git commit count) is automatic.
2. **Commit** everything first — the build number is baked at CMake configure time,
   so the version stamp only matches `HEAD` after a build that follows the commit.
3. **Build**:
   ```cmd
   scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON
   scripts\build-installer.cmd
   ```
   → `build\installer\RabbitEars-<ver>-setup.exe` (bundles the exe, libVLC DLLs +
   the whole `plugins\` tree, WinSparkle, and LICENSE).
4. **Sign the installer** with the private key and copy the printed
   `sparkle:edSignature`:
   - macOS: `./bin/sign_update build/installer/RabbitEars-<ver>-setup.exe`
   - Windows: `sign_update.exe build\installer\RabbitEars-<ver>-setup.exe`
5. **Generate the appcast** (writes `appcast.xml` at the repo root):
   ```cmd
   pwsh scripts\make-appcast.ps1 -Version 0.1.1.<build> ^
        -SetupExe build\installer\RabbitEars-0.1.1-setup.exe -Signature <sig>
   ```
   (`-Version` must equal `RE_VERSION_FULL` of that build, i.e. `APP_VERSION.BUILD`.)
6. **Publish on GitHub**: create a Release tagged `v0.1.1` (or `v<ver>`), upload the
   setup `.exe` as an asset (the appcast enclosure URL points there).
7. **Commit & push `appcast.xml`** on `main` — `raw.githubusercontent.com` serves it
   at the feed URL.

Installed apps pick it up via the About box's **Check for Updates…** and
WinSparkle's periodic check.

## Not yet covered
- **Authenticode signing** of the exe + installer is not set up. Without it,
  SmartScreen may warn on first download/run. (The EdDSA signature above is the
  *update-integrity* signature WinSparkle verifies; it is separate from
  Authenticode.)
- Keep the Ed25519 **private key out of the repo**.
