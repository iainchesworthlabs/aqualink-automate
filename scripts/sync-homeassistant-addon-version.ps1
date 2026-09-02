#!/usr/bin/env pwsh
#
# Home Assistant add-on version single-source (docs/design/homeassistant-addon.md).
#
# The add-on wraps the app image published to GHCR. The Dockerfile's base is
# `ghcr.io/iainchesworthlabs/aqualink-automate:${BUILD_VERSION}`, where BUILD_VERSION IS the
# add-on's config.yaml `version` (Supervisor-provided on a local build; CI + the dev
# harness pass it explicitly). So the ONLY version to keep aligned is config.yaml
# `version` == the app release version — there is no longer a separate build.yaml tag.
#
# Two channels (docs/design/homeassistant-addon.md):
#   stable -> aqualink-automate       (tracks the latest stable release)
#   edge   -> aqualink-automate-edge   (tracks the latest prerelease)
# A stable release bumps stable; a prerelease bumps edge.
#
# Usage:
#   Set   : ./scripts/sync-homeassistant-addon-version.ps1 -Channel edge -Version 0.14.0-beta.1 [-Root <repo>]
#   Check : ./scripts/sync-homeassistant-addon-version.ps1 -Check [-Channel <c>] [-Version <expected>] [-Root <repo>]
#
#   -Version alone            : rewrite that channel's config.yaml `version`.
#   -Check                    : read the version(s); with no -Channel, both channels.
#   -Check -Channel -Version  : require that channel's version == <expected> AND that
#                               its CHANGELOG.md has a matching '## <expected>' entry
#                               (release guard).
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

# Durable anchor: the config `version:` value.
$configVersionRe = '(?m)^(?<pre>version:\s*")(?<ver>[^"]*)(?<post>")\s*$'

# Durable anchor: a CHANGELOG.md `## <version>` heading (no brackets/date -- that is
# the repo-root CHANGELOG.md's format, not this per-add-on one).
$changelogHeadingRe = '(?m)^##\s+(?<ver>\S+)\s*$'

# Write with UNIX (LF) line endings and no BOM, so output is byte-identical on Windows
# and Linux (the edge no-drift check runs on Linux CI).
function Write-Lf([string]$path, [string]$text) {
    $lf = ($text -replace "`r`n", "`n") -replace "`r", "`n"
    [System.IO.File]::WriteAllText($path, $lf, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-ChannelVersion([string]$dir, [string]$version) {
    $configPath = Join-Path $dir 'config.yaml'
    $configText = Get-Content -Raw -LiteralPath $configPath
    if ($configText -notmatch $configVersionRe) { Write-Error "No 'version:' key in $configPath"; exit 1 }
    $newConfig = [regex]::Replace($configText, $configVersionRe, { param($m) $m.Groups['pre'].Value + $version + $m.Groups['post'].Value })
    if ($newConfig -ne $configText) { Write-Lf $configPath $newConfig }
    Write-Host "Synced '$dir' to version $version"
}

# Returns $true when the channel's version is present (and == $expected if given).
function Test-ChannelVersion([string]$name, [string]$dir, [string]$expected) {
    $configPath = Join-Path $dir 'config.yaml'
    $cm = [regex]::Match((Get-Content -Raw -LiteralPath $configPath), $configVersionRe)
    if (-not $cm.Success) { Write-Error "Could not read 'version:' from $configPath"; exit 1 }
    $configVersion = $cm.Groups['ver'].Value
    Write-Host "[$name] config.yaml version : $configVersion"
    if ($expected -and ($configVersion -ne $expected)) {
        Write-Host "::error::[$name] add-on version '$configVersion' != expected release version '$expected'"
        Write-Host "         Run: ./scripts/sync-homeassistant-addon-version.ps1 -Channel $name -Version $expected"
        return $false
    }
    return $true
}

# The config.yaml version bump and the CHANGELOG.md entry are two separate manual
# edits to the same channel -- config.yaml alone tells the Supervisor an update
# exists, but the Changelog tab in its update dialog reads CHANGELOG.md, so a
# missed entry there ships a real update with a stale/absent changelog. Returns
# $true when the channel's CHANGELOG.md has a heading matching $expected.
function Test-ChannelChangelog([string]$name, [string]$dir, [string]$expected) {
    $changelogPath = Join-Path $dir 'CHANGELOG.md'
    if (-not (Test-Path $changelogPath)) { Write-Error "No CHANGELOG.md in $dir"; exit 1 }
    $headings = [regex]::Matches((Get-Content -Raw -LiteralPath $changelogPath), $changelogHeadingRe) |
        ForEach-Object { $_.Groups['ver'].Value }
    if ($expected -notin $headings) {
        Write-Host "::error::[$name] CHANGELOG.md has no '## $expected' entry"
        Write-Host "         Add one to $changelogPath before releasing (edge is regenerated from stable -- see scripts/gen-homeassistant-edge-addon.ps1)."
        return $false
    }
    return $true
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
    if ($expected -and -not (Test-ChannelChangelog $name $dir $expected)) { $ok = $false }
}

if (-not $ok) { exit 1 }
Write-Host "Home Assistant add-on version is consistent."
exit 0
