# SPDX-License-Identifier: GPL-3.0-or-later
# Generate the WinSparkle appcast.xml (repo root) for a release. The installer must
# already be signed with the EdDSA private key; pass the resulting
# sparkle:edSignature here. Computes the enclosure length + pubDate.
#
#   pwsh scripts\make-appcast.ps1 -Version 0.1.0.43 `
#        -SetupExe build\installer\RabbitEars-0.1.0-setup.exe -Signature <base64>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,    # marketing.build, e.g. 0.1.0.43 (matches RE_VERSION_FULL)
    [Parameter(Mandatory)][string]$SetupExe,   # path to the built (and signed) installer
    [Parameter(Mandatory)][string]$Signature,  # sparkle:edSignature from sign_update
    [string]$Repo = 'arcanii/RabbitEars',
    [string]$Tag,                              # GitHub release tag; defaults to v$Version
    [string]$Notes,                            # release-notes URL; defaults to the release page
    [ValidateSet('x64','arm64')][string]$Arch = 'x64'  # x64 -> appcast.xml, arm64 -> appcast-arm64.xml
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $SetupExe)) { throw "Setup exe not found: $SetupExe" }
if (-not $Tag) { $Tag = "v$Version" }
$len = (Get-Item -LiteralPath $SetupExe).Length
$file = Split-Path -Path $SetupExe -Leaf
$url = "https://github.com/$Repo/releases/download/$Tag/$file"
if (-not $Notes) { $Notes = "https://github.com/$Repo/releases/tag/$Tag" }
$pub = (Get-Date).ToUniversalTime().ToString(
    'ddd, dd MMM yyyy HH:mm:ss +0000', [System.Globalization.CultureInfo]::InvariantCulture)
$feed  = if ($Arch -eq 'arm64') { 'appcast-arm64.xml' } else { 'appcast.xml' }
$title = if ($Arch -eq 'arm64') { 'RabbitEars (Win32 ARM64) Updates' } else { 'RabbitEars (Win32) Updates' }
$desc  = if ($Arch -eq 'arm64') { 'Most recent updates to RabbitEars for Windows on ARM64' } else { 'Most recent updates to RabbitEars for Windows' }
$out = Join-Path (Split-Path -Path $PSScriptRoot -Parent) $feed
$xml = @"
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>$title</title>
    <link>https://raw.githubusercontent.com/$Repo/main/$feed</link>
    <description>$desc</description>
    <language>en</language>
    <item>
      <title>Version $Version</title>
      <sparkle:releaseNotesLink>$Notes</sparkle:releaseNotesLink>
      <pubDate>$pub</pubDate>
      <enclosure url="$url"
                 sparkle:version="$Version"
                 sparkle:edSignature="$Signature"
                 length="$len"
                 type="application/octet-stream"/>
    </item>
  </channel>
</rss>
"@
# Write UTF-8 WITHOUT a BOM, deterministically across Windows PowerShell 5.1 (whose
# `Set-Content -Encoding UTF8` emits a BOM) and pwsh 7 — the appcast is a clean UTF-8 XML feed.
[System.IO.File]::WriteAllText($out, $xml + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote $out"
Write-Host "  version=$Version  arch=$Arch  length=$len  url=$url"
Write-Host "Next: commit $feed on main, and upload $file to the '$Tag' GitHub release."
