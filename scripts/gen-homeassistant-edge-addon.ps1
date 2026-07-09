#!/usr/bin/env pwsh
#
# Generate the Edge (beta) Home Assistant add-on from the stable one.
#
# The two channels (docs/design/homeassistant-addon.md) are near-identical add-ons:
#   - aqualink-automate/       — stable channel (the single source of truth)
#   - aqualink-automate-edge/  — GENERATED beta channel (this script's output)
#
# They differ ONLY in identity (name/slug/stage/panel_title) and in the version they
# track (stable → latest stable release; edge → latest prerelease). Everything else
# (run.sh, Dockerfile, translations, DOCS) is copied verbatim, so the channels can
# never drift. The `Home Assistant Add-on` CI job runs this script and fails on any
# uncommitted diff, so the edge folder must always be the generator's output.
#
# The edge VERSION is managed independently (by sync-homeassistant-addon-version.ps1
# -Channel edge) and PRESERVED here across regenerations — this script only touches
# structure, never bumps the version.
#
# Usage:  ./scripts/gen-homeassistant-edge-addon.ps1 [-Root <repo-root>]
# Exit:   0 = generated.

param(
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

$src  = Join-Path $Root 'aqualink-automate'
$dest = Join-Path $Root 'aqualink-automate-edge'

if (-not (Test-Path (Join-Path $src 'config.yaml'))) {
    Write-Error "Stable add-on not found at $src"
    exit 1
}

function Get-Version([string]$configPath) {
    if (-not (Test-Path $configPath)) { return $null }
    $m = [regex]::Match((Get-Content -Raw -LiteralPath $configPath), '(?m)^version:\s*"([^"]*)"')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}

# Preserve the edge channel's own version across regenerations; seed from stable on
# first creation.
$srcVersion  = Get-Version (Join-Path $src  'config.yaml')
$edgeVersion = Get-Version (Join-Path $dest 'config.yaml')
if (-not $edgeVersion) { $edgeVersion = $srcVersion }

# Rebuild the edge tree from scratch so a file deleted in stable also disappears here.
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Copy-Item -Recurse -LiteralPath $src -Destination $dest

# Write text with UNIX (LF) line endings and no BOM, so the generator's output is
# byte-identical on Windows and the Linux CI runner (the no-drift check runs on Linux).
# PowerShell here-strings/Set-Content would otherwise emit CRLF on Windows.
function Write-Lf([string]$path, [string]$text) {
    $lf = ($text -replace "`r`n", "`n") -replace "`r", "`n"
    [System.IO.File]::WriteAllText($path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}

$banner = @"
# GENERATED from ../aqualink-automate by scripts/gen-homeassistant-edge-addon.ps1 — DO NOT EDIT.
# Change the stable add-on, then re-run the generator and commit (CI enforces no drift).
"@

# ── config.yaml: identity + version overrides ───────────────────────────────────
$configPath = Join-Path $dest 'config.yaml'
$cfg = Get-Content -Raw -LiteralPath $configPath
$cfg = $cfg -replace '(?m)^name:\s*.*$',        'name: Aqualink Automate (Edge)'
$cfg = $cfg -replace '(?m)^slug:\s*.*$',        'slug: aqualink_automate_edge'
$cfg = $cfg -replace '(?m)^stage:\s*.*$',       'stage: experimental'
$cfg = $cfg -replace '(?m)^panel_title:\s*.*$', 'panel_title: Aqualink (Edge)'
$cfg = $cfg -replace '(?m)^version:\s*"[^"]*"', ('version: "' + $edgeVersion + '"')
Write-Lf $configPath ($banner + "`n" + $cfg)

# ── build.yaml: base-image tag == edge version ──────────────────────────────────
$buildPath = Join-Path $dest 'build.yaml'
$bld = Get-Content -Raw -LiteralPath $buildPath
$bld = [regex]::Replace($bld, '(ghcr\.io/[^"'':\s]+:)[^"''\s]+', { param($m) $m.Groups[1].Value + $edgeVersion })
Write-Lf $buildPath ($banner + "`n" + $bld)

# ── apparmor.txt(.draft): the AppArmor profile name must be the edge slug, so the
#    stable and edge channels don't collide on one host when both are active. ──────
foreach ($aa in @('apparmor.txt', 'apparmor.txt.draft')) {
    $aaPath = Join-Path $dest $aa
    if (Test-Path $aaPath) {
        $aaText = Get-Content -Raw -LiteralPath $aaPath
        $aaText = $aaText -replace '(?m)^(profile\s+)aqualink_automate(\s+flags=)', ('${1}aqualink_automate_edge${2}')
        Write-Lf $aaPath $aaText
    }
}

# ── README: a short generated/beta note on top ──────────────────────────────────
$readmePath = Join-Path $dest 'README.md'
if (Test-Path $readmePath) {
    $readme = Get-Content -Raw -LiteralPath $readmePath
    $note = "<!-- GENERATED from ../aqualink-automate — do not edit; see scripts/gen-homeassistant-edge-addon.ps1 -->`n`n> **Edge (beta) channel.** Tracks the newest prerelease. For the stable channel install **Aqualink Automate** instead.`n`n"
    Write-Lf $readmePath ($note + $readme)
}

Write-Host "Generated edge add-on at $dest (version $edgeVersion)"
