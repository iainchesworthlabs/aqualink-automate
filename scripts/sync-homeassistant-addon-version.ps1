#!/usr/bin/env pwsh
#
# Home Assistant add-on version lock-step (docs/design/homeassistant-addon.md).
#
# The add-on is a thin wrapper over the multi-arch image release.yml publishes to
# GHCR. For that to work, THREE versions must agree PER CHANNEL:
#
#   1. <channel>/config.yaml : version
#   2. <channel>/build.yaml  : every build_from tag
#      (ghcr.io/iainchesworth/aqualink-automate:<tag>)
#   3. the app release version the image was published under (no leading 'v')
#
# There are two channels (docs/design/homeassistant-addon.md):
#   stable -> aqualink-automate       (tracks the latest stable release)
#   edge   -> aqualink-automate-edge   (tracks the latest prerelease)
#
# A stable release bumps the stable channel; a prerelease bumps edge. This script is
# the single writer that keeps (1)==(2) aligned and the checker CI/release run.
#
# Usage:
#   Set   : ./scripts/sync-homeassistant-addon-version.ps1 -Channel edge -Version 0.14.0-beta.1 [-Root <repo>]
#   Check : ./scripts/sync-homeassistant-addon-version.ps1 -Check [-Channel <c>] [-Version <expected>] [-Root <repo>]
#
#   -Channel          : stable (default) or edge — which channel folder to act on.
#   -Version alone    : rewrite that channel's config.yaml + build.yaml to the version.
#   -Check            : verify config.yaml == every build.yaml tag. With no -Channel,
#                       checks EVERY channel that exists (stable + edge).
#   -Check -Channel -Version : additionally require that channel == <expected> (release).
#
# Exit: 0 = ok / written, 1 = drift or bad input.

param(
    [string]$Version,
    [switch]$Check,
    [ValidateSet('stable', 'edge')]
    [string]$Channel,
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

$channelDirs = [ordered]@{
    stable = Join-Path $Root 'aqualink-automate'
    edge   = Join-Path $Root 'aqualink-automate-edge'
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

# Write with UNIX (LF) line endings and no BOM, so output is byte-identical on Windows
# and Linux (the edge no-drift check runs on Linux CI).
function Write-Lf([string]$path, [string]$text) {
    $lf = ($text -replace "`r`n", "`n") -replace "`r", "`n"
    [System.IO.File]::WriteAllText($path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-ChannelVersion([string]$dir, [string]$version) {
    $configPath = Join-Path $dir 'config.yaml'
    $buildPath  = Join-Path $dir 'build.yaml'
    $configText = Get-Content -Raw -LiteralPath $configPath
    $buildText  = Get-Content -Raw -LiteralPath $buildPath

    if ($configText -notmatch $configVersionRe) { Write-Error "No 'version:' key in $configPath"; exit 1 }
    if ($buildText  -notmatch $buildTagRe)       { Write-Error "No ghcr.io build_from image in $buildPath"; exit 1 }

    $newConfig = [regex]::Replace($configText, $configVersionRe, { param($m) $m.Groups['pre'].Value + $version + $m.Groups['post'].Value })
    $newBuild  = [regex]::Replace($buildText,  $buildTagRe,      { param($m) $m.Groups['pre'].Value + $version })

    if ($newConfig -ne $configText) { Write-Lf $configPath $newConfig }
    if ($newBuild  -ne $buildText)  { Write-Lf $buildPath  $newBuild }
    Write-Host "Synced '$dir' to version $version"
}

# Returns $true when the channel is internally consistent (and == $expected if given).
function Test-ChannelVersion([string]$name, [string]$dir, [string]$expected) {
    $configPath = Join-Path $dir 'config.yaml'
    $buildPath  = Join-Path $dir 'build.yaml'
    $configText = Get-Content -Raw -LiteralPath $configPath
    $buildText  = Get-Content -Raw -LiteralPath $buildPath

    $cm = [regex]::Match($configText, $configVersionRe)
    if (-not $cm.Success) { Write-Error "Could not read 'version:' from $configPath"; exit 1 }
    $configVersion = $cm.Groups['ver'].Value

    $tags = [regex]::Matches($buildText, $buildTagRe) | ForEach-Object { $_.Groups['ver'].Value }
    if ($tags.Count -eq 0) { Write-Error "Could not read any build_from image tag from $buildPath"; exit 1 }

    $ok = $true
    Write-Host "[$name] config.yaml version : $configVersion"
    foreach ($t in $tags) {
        if ($t -ne $configVersion) {
            Write-Host "::error::[$name] build.yaml image tag '$t' != config.yaml version '$configVersion'"
            $ok = $false
        } else {
            Write-Host "[$name] build.yaml tag      : $t (match)"
        }
    }
    if ($expected -and ($configVersion -ne $expected)) {
        Write-Host "::error::[$name] add-on version '$configVersion' != expected release version '$expected'"
        Write-Host "         Run: ./scripts/sync-homeassistant-addon-version.ps1 -Channel $name -Version $expected"
        $ok = $false
    }
    return $ok
}

# ── Set mode ────────────────────────────────────────────────────────────────────
if ($Version -and -not $Check) {
    $ch = if ($Channel) { $Channel } else { 'stable' }
    Set-ChannelVersion $channelDirs[$ch] $Version
    exit 0
}

# ── Check mode ──────────────────────────────────────────────────────────────────
if ($Version -and $Check -and -not $Channel) {
    Write-Error "-Check -Version requires -Channel (which channel must equal <expected>)."
    exit 1
}

$channelsToCheck = if ($Channel) { @($Channel) } else { @('stable', 'edge') }
$ok = $true
foreach ($name in $channelsToCheck) {
    $dir = $channelDirs[$name]
    if (-not (Test-Path (Join-Path $dir 'config.yaml'))) {
        if ($Channel) { Write-Error "Channel '$name' not found at $dir"; exit 1 }
        continue  # a channel that does not exist yet is fine when scanning all
    }
    $expected = if ($Channel -and $Version) { $Version } else { $null }
    if (-not (Test-ChannelVersion $name $dir $expected)) { $ok = $false }
}

if (-not $ok) { exit 1 }
Write-Host "Home Assistant add-on version is in lock-step."
exit 0
