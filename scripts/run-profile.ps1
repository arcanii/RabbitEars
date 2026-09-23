# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run a RabbitEars build as a separate PROFILE, beside the installed app - e.g. to check a dev build
# against the release on the same machine, side by side.
#
#   scripts\run-profile.ps1                  # the dev build (build\Win32\RabbitEars.exe), profile "dev"
#   scripts\run-profile.ps1 -Refresh         # re-take the library snapshot first
#   scripts\run-profile.ps1 -Name b -Exe X   # another profile / another exe
#
# A profile is RABBITEARS_DATA_DIR pointed at %LOCALAPPDATA%\RabbitEarsProfiles\<Name>: its own
# database, settings and log, its own single-instance lock, and <Name> tagged onto the
# title bar. It never touches the Windows wake-to-record task (that belongs to the installed app). See
# Win32/platform/Profile.h for what is - and is not - separated.
#
# The profile's database is a SNAPSHOT of your real one (SQLite's online backup, so it is consistent
# even while the installed app is running), taken the first time and on -Refresh. Scheduled
# recordings and recording rules are REMOVED from the snapshot, so the two instances never record the
# same programme against a one-connection line. The real database is opened read-only (SQLite may
# still create its empty -wal/-shm side files beside it, exactly as the app does).
# The exe must be a build that understands profiles (0.2.18+): an older one would ignore the profile,
# take the normal single-instance lock and clear the installed app's wake-to-record task, so the
# script refuses it. Needs Python 3 (already required by the i18n tooling).
param(
    [string]$Name = "dev",
    [string]$Exe = (Join-Path $PSScriptRoot "..\build\Win32\RabbitEars.exe"),
    [switch]$Refresh
)
$ErrorActionPreference = "Stop"

# A plain folder name only: anything else could resolve to the REAL data dir, and -Refresh would then
# replace the real database with a stripped snapshot.
if ($Name -notmatch '^[A-Za-z0-9_.-]+$' -or $Name -eq '.' -or $Name -eq '..') {
    throw "-Name must be a plain folder name (letters, digits, _ . -): '$Name'"
}
$Exe = (Resolve-Path $Exe).Path
$real = Join-Path $env:LOCALAPPDATA "RabbitEars\rabbitears.db"
$root = Join-Path $env:LOCALAPPDATA "RabbitEarsProfiles"
$dir = Join-Path $root $Name
$db = Join-Path $dir "rabbitears.db"

# Refuse a build that predates profiles. Profile.h's mutex format string is in every build that
# supports them (UTF-16 in the exe); search both byte alignments.
$bytes = [System.IO.File]::ReadAllBytes($Exe)
$marker = "RabbitEars.SingleInstance.%016llx"
$u16 = [System.Text.Encoding]::Unicode
if (-not ($u16.GetString($bytes).Contains($marker) -or $u16.GetString($bytes, 1, $bytes.Length - 1).Contains($marker))) {
    throw "$Exe predates profiles (0.2.18+): it would take the normal instance lock and clear the installed app's wake task."
}

if ($Refresh -or -not (Test-Path $db)) {
    if (-not (Test-Path $real)) { throw "No library to snapshot at $real" }
    # Replacing the database under a running profile would corrupt it.
    $running = Get-Process RabbitEars -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }
    if ($running) { throw "Close the running $Exe first (pid $($running.Id -join ', ')) - it may be this profile." }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $tmp = "$db.snapshot"
    Write-Host "Snapshotting $real -> $db ..."
    $py = @'
import pathlib, sqlite3, sys
src_path, dst_path = sys.argv[1], sys.argv[2]
src = sqlite3.connect(pathlib.Path(src_path).as_uri() + "?mode=ro", uri=True)
dst = sqlite3.connect(dst_path)
src.backup(dst)
src.close()
removed = {}
for table in ("scheduled_recordings", "recording_rules"):
    try:
        removed[table] = dst.execute(f"DELETE FROM {table}").rowcount
    except sqlite3.OperationalError:
        removed[table] = 0  # an older schema without it
dst.commit()
rows = dst.execute("SELECT COUNT(*) FROM channels").fetchone()[0]
dst.close()
print(f"  {rows} channels; removed {removed['scheduled_recordings']} scheduled recordings, "
      f"{removed['recording_rules']} rules from the snapshot")
'@
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
    $py | python - $real $tmp
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $tmp)) { throw "Snapshot failed (python exit $LASTEXITCODE)" }
    foreach ($f in @($db, "$db-wal", "$db-shm")) { if (Test-Path $f) { Remove-Item -Force $f } }
    Move-Item $tmp $db
}

# CreateProcess directly (UseShellExecute = false), with the variable set in the CHILD's environment
# only. Not Start-Process: that goes through the shell, and for an exe on a network drive (the dev
# tree lives on one) the shell first shows an "Open File - Security Warning" and blocks until someone
# answers it - which looked like the script hanging for minutes.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.WorkingDirectory = Split-Path $Exe
$psi.UseShellExecute = $false
$psi.Environment["RABBITEARS_DATA_DIR"] = $dir
$p = [System.Diagnostics.Process]::Start($psi)
Write-Host "Started $Exe as profile '$Name' (pid $($p.Id)), data in $dir"
