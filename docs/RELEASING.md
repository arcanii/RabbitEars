# Releasing RabbitEars (Win32)

Auto-update uses **WinSparkle** reading an **EdDSA-signed appcast hosted on
GitHub**, mirroring the sibling apps (SQLTerminal-Win32 / ManorLords-SGE) and the
macOS Sparkle setup. RabbitEars **shares the family Ed25519 key pair**: the public
key in `Win32/platform/Updater.cpp` (`win_sparkle_set_eddsa_public_key`) is the same
string as the macOS `SUPublicEDKey`, so **the same private key signs Windows
packages** and signing is done **on macOS** with Sparkle's `sign_update`.
(To isolate RabbitEars, generate a dedicated key pair and paste its public key
into `Updater.cpp` instead — everything else is unchanged.)

- Feeds — **one per architecture** (WinSparkle can't pick an enclosure by arch, so each
  build reads its own feed and downloads its own installer; an ARM user always gets the
  native build, never the x64 one). The build selects its feed at compile time in
  `Win32/platform/Updater.cpp` (`#ifdef _M_ARM64`):
  - x64:   `https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast.xml`
  - ARM64: `https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast-arm64.xml`
- Installers: GitHub Release assets — `RabbitEars-<ver>-setup.exe` (x64) and, when shipping
  the native build, `RabbitEars-<ver>-arm64-setup.exe` (ARM64). The two arches can release
  independently: if a version ships x64-only, just skip the ARM64 track and its appcast keeps
  pointing at the last ARM64 build.
- Optional **universal** first-download — `RabbitEars-<ver>-universal-setup.exe` bundles BOTH
  binary sets and installs the NATIVE arch at install time (Inno `ProcessorArchitecture` Check),
  so a user who doesn't know their arch gets the right build. It needs **no appcast of its own**:
  the installed result is byte-identical to a per-arch install, so it auto-updates through the
  same per-arch feed. Sign it too (WinSparkle never downloads it, but a signature is cheap
  insurance); it does not appear in any appcast enclosure.

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
   `APP_VERSION` in `cmake/AppVersion.cmake` (About box + update checks; the
   `if(APPLE)` override below it is the mac version and must NOT move), `MyVer` in
   `packaging/installer.iss` (installer name/version), the `FILEVERSION`/
   `PRODUCTVERSION` + `FileVersion`/`ProductVersion` strings in
   `packaging/RabbitEars.rc`, and the `assemblyIdentity version` in
   `packaging/app.manifest`. The build number (git commit count) is automatic.
2. **Commit** everything first — the build number is baked at CMake configure time,
   so the version stamp only matches `HEAD` after a build that follows the commit.
3. **Build both arches** (skip the ARM64 lines for an x64-only release):
   ```cmd
   scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON            :: x64   -> build\Win32\
   scripts\build-installer.cmd                            :: x64 installer
   scripts\build-arm64.cmd -DRABBITEARS_THEME_ENGINE=ON   :: ARM64 -> build-arm64\Win32\
   scripts\build-installer.cmd arm64                      :: ARM64 installer
   scripts\build-installer.cmd universal                  :: universal (needs BOTH build dirs)
   ```
   → `build\installer\RabbitEars-<ver>-setup.exe`, `RabbitEars-<ver>-arm64-setup.exe`, and
   `RabbitEars-<ver>-universal-setup.exe` (each per-arch installer bundles that arch's exe, libVLC
   DLLs + the whole `plugins\` tree, WinSparkle, LICENSE; the universal one bundles both sets).
   The ARM64 installer sources from `build-arm64\` and is `ArchitecturesAllowed=arm64` (native
   only); the x64 one stays `x64compatible` (runs on x64 and, emulated, on ARM); the universal one
   allows both and picks the native set at install time.
4. **Sign each installer** with the private key and copy the printed `sparkle:edSignature`
   (one signature per file):
   - macOS (copy the installer over first): `scripts/sign-release.sh RabbitEars-<ver>-setup.exe`
     — wraps Sparkle's `sign_update` (login-Keychain key) and prints just the signature.
   - Windows (if the key is there): `sign_update.exe build\installer\RabbitEars-<ver>-setup.exe`
5. **Generate the appcast(s)** at the repo root — one per arch you built:
   ```cmd
   pwsh scripts\make-appcast.ps1 -Version <ver.build> ^
        -SetupExe build\installer\RabbitEars-<ver>-setup.exe -Signature <sig-x64>
   pwsh scripts\make-appcast.ps1 -Arch arm64 -Version <ver.build> ^
        -SetupExe build\installer\RabbitEars-<ver>-arm64-setup.exe -Signature <sig-arm64>
   ```
   (`-Version` must equal `RE_VERSION_FULL` of that build, i.e. `APP_VERSION.BUILD`; the plain
   form writes `appcast.xml`, `-Arch arm64` writes `appcast-arm64.xml`.)
6. **Publish on GitHub**: create a Release tagged `v<ver>`, upload **both** setup `.exe`s as
   assets (each appcast's enclosure URL points at its own file in this release).
7. **Commit & push** the appcast(s) you regenerated (`appcast.xml` and/or `appcast-arm64.xml`)
   on `main` — `raw.githubusercontent.com` serves them at the feed URLs.

Installed apps pick it up via the About box's **Check for Updates…** and
WinSparkle's periodic check.

## Not yet covered
- **Authenticode signing** of the exe + installer is not set up. Without it, SmartScreen
  may warn on first download/run. (The EdDSA signature above is the *update-integrity*
  signature WinSparkle verifies; it is separate from Authenticode.) **Owner-gated — needs a
  code-signing certificate purchase.** When one exists, the wiring is:
  1. Buy an **OV code-signing cert** (EV clears SmartScreen fastest; OV builds reputation
     over downloads). Modern CA rules require the key in hardware (USB token) or a cloud
     signer — **Azure Trusted Signing** is the low-friction option and works headless.
  2. Sign the **exe before packaging**, then the **installer** (both, so the installed
     binary and the download are trusted):
     ```cmd
     signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
              /n "<cert subject>" build\Win32\RabbitEars.exe
     scripts\build-installer.cmd            (re-pack with the signed exe)
     signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 ^
              /n "<cert subject>" build\installer\RabbitEars-<ver>-setup.exe
     ```
     (with Azure Trusted Signing, swap `/n` for the `/dlib`+`/dmdf` metadata flags.)
     Slot these between steps 3 and 4 of "Per release" — Authenticode FIRST, then the
     EdDSA `sign_update` signature (which covers the final signed bytes).
  3. Always **timestamp** (`/tr`) so signatures outlive the cert's validity.
  4. `signtool` ships in the Windows SDK (already present — the build uses `fxc` from it).
- Keep the Ed25519 **private key out of the repo**.
