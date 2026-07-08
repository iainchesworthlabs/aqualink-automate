#!/usr/bin/env pwsh
#
# Home Assistant add-on version lock-step (docs/design/homeassistant-addon.md).
#
# The add-on is a thin wrapper over the multi-arch image release.yml publishes to
# GHCR. For that to work, THREE versions must agree:
#
#   1. homeassistant/aqualink-automate/config.yaml : version
#   2. homeassistant/aqualink-automate/build.yaml  : every build_from tag
#      (ghcr.io/iainchesworth/aqualink-automate:<tag>)
#   3. the app release version the image was published under (no leading 'v')
#
# (1) == (2) is what the user sees vs the image that is actually pulled; (2) == (3)
# is that the referenced image exists. This script is the single writer that keeps
# them aligned and the checker CI runs to catch drift.
#
# Usage:
#   Set   : ./scripts/sync-homeassistant-addon-version.ps1 -Version 0.12.0-beta.5 [-Root <repo>]
#   Check : ./scripts/sync-homeassistant-addon-version.ps1 -Check [-Version <expected>] [-Root <repo>]
#
#   -Version alone  : rewrite config.yaml + build.yaml to that version.
#   -Check          : verify config.yaml == every build.yaml tag (internal consistency).
#   -Check -Version : additionally require they equal <expected> (used at release time).
#
# Exit: 0 = ok / written, 1 = drift or bad input.

param(
    [string]$Version,
    [switch]$Check,
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

$configPath = Join-Path $Root 'homeassistant/aqualink-automate/config.yaml'
$buildPath  = Join-Path $Root 'homeassistant/aqualink-automate/build.yaml'

foreach ($p in @($configPath, $buildPath)) {
    if (-not (Test-Path $p)) {
        Write-Error "Add-on manifest not found: $p"
        exit 1
    }
}

# Accepted release version (no leading 'v'): M.M.P[-(alpha|beta|rc).N], mirroring
# release.yml's resolve-version regex.
$semverPattern = '^[0-9]+\.[0-9]+\.[0-9]+(-(alpha|beta|rc)\.[0-9]+)?$'

if ($Version -and ($Version -notmatch $semverPattern)) {
    Write-Error "Invalid -Version '$Version' (expected M.M.P[-(alpha|beta|rc).N], no leading 'v')."
    exit 1
}

# Durable anchors, not line numbers: the config `version:` value and each build_from
# image tag (the part after the final ':' before the closing quote).
$configVersionRe = '(?m)^(?<pre>version:\s*")(?<ver>[^"]*)(?<post>")\s*$'
$buildTagRe      = '(?<pre>ghcr\.io/[^"'':\s]+:)(?<ver>[^"''\s]+)'

$configText = Get-Content -Raw -LiteralPath $configPath
$buildText  = Get-Content -Raw -LiteralPath $buildPath

# ── Set mode ────────────────────────────────────────────────────────────────────
if ($Version -and -not $Check) {
    $cm = [regex]::Match($configText, $configVersionRe)
    if (-not $cm.Success) { Write-Error "Could not find a 'version:' key in $configPath"; exit 1 }
    $newConfig = [regex]::Replace($configText, $configVersionRe, { param($m) $m.Groups['pre'].Value + $Version + $m.Groups['post'].Value })

    if ($buildText -notmatch $buildTagRe) { Write-Error "Could not find a ghcr.io build_from image in $buildPath"; exit 1 }
    $newBuild = [regex]::Replace($buildText, $buildTagRe, { param($m) $m.Groups['pre'].Value + $Version })

    if ($newConfig -ne $configText) { Set-Content -NoNewline -LiteralPath $configPath -Value $newConfig }
    if ($newBuild  -ne $buildText)  { Set-Content -NoNewline -LiteralPath $buildPath  -Value $newBuild }

    Write-Host "Synced Home Assistant add-on to version $Version"
    Write-Host "  config.yaml : $configPath"
    Write-Host "  build.yaml  : $buildPath"
    exit 0
}

# ── Check mode (default when -Version is absent, or explicit -Check) ─────────────
$configMatch = [regex]::Match($configText, $configVersionRe)
if (-not $configMatch.Success) { Write-Error "Could not read 'version:' from $configPath"; exit 1 }
$configVersion = $configMatch.Groups['ver'].Value

$buildTags = [regex]::Matches($buildText, $buildTagRe) | ForEach-Object { $_.Groups['ver'].Value }
if ($buildTags.Count -eq 0) { Write-Error "Could not read any build_from image tag from $buildPath"; exit 1 }

$ok = $true
Write-Host "config.yaml version : $configVersion"
foreach ($t in $buildTags) {
    if ($t -ne $configVersion) {
        Write-Host "::error::build.yaml image tag '$t' != config.yaml version '$configVersion'"
        $ok = $false
    } else {
        Write-Host "build.yaml tag      : $t (match)"
    }
}

if ($Version -and ($configVersion -ne $Version)) {
    Write-Host "::error::add-on version '$configVersion' != expected release version '$Version'"
    Write-Host "         Run: ./scripts/sync-homeassistant-addon-version.ps1 -Version $Version"
    $ok = $false
}

if (-not $ok) { exit 1 }
Write-Host "Home Assistant add-on version is in lock-step."
exit 0
