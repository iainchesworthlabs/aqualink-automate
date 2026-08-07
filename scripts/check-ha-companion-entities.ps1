# Validates the Home Assistant companion package's entity references.
#
# Every ACTIVE (non-comment) reference to an app-published entity
# (<platform>.aqualink_automate_<slug>) in homeassistant/companion/**/*.yaml must
# appear in homeassistant/companion/entity-manifest.json. Panel-label-driven
# (dynamic) entities have install-specific ids and must only be referenced in
# commented examples or supplied by users through blueprint inputs — this check
# is what enforces that rule (see homeassistant/companion/README.md).
#
# The manifest itself is kept in lock-step with src/core/mqtt/ha_discovery.cpp by
# TestSuite_HaDiscovery_CompanionManifest in test/unit/mqtt/test_mqtt_ha_discovery.cpp.
#
# Usage: pwsh scripts/check-ha-companion-entities.ps1 [-Root <repo root>]
# Exit code 0 = clean, 1 = unknown entity reference (or a broken manifest).

param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

$companionDir = Join-Path $Root "homeassistant/companion"
$manifestPath = Join-Path $companionDir "entity-manifest.json"

if (-not (Test-Path $manifestPath)) {
    Write-Host "::error file=$manifestPath::entity manifest not found"
    exit 1
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$validIds = @($manifest.static_entities | ForEach-Object { $_.entity_id })

# Guard against an accidentally truncated manifest silently passing everything.
if ($validIds.Count -lt 20) {
    Write-Host "::error file=$manifestPath::manifest lists only $($validIds.Count) static entities - expected the full contract (>= 20)"
    exit 1
}

$validSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$validIds)

# Match app-published entity ids only: helper entities created by the companion
# package deliberately use the `aqualink_` (not `aqualink_automate_`) prefix and
# are out of scope here.
$refPattern = '(?:sensor|binary_sensor|switch|number|select)\.aqualink_automate_[a-z0-9_]+'

$failures = 0
$checkedFiles = 0
$activeRefs = 0

$yamlFiles = Get-ChildItem -Path $companionDir -Recurse -Include *.yaml, *.yml -File |
    Where-Object { $_.FullName -notmatch '[\\/]test-harness[\\/]' }

foreach ($file in $yamlFiles) {
    $checkedFiles++
    $lineNo = 0
    foreach ($line in Get-Content $file.FullName) {
        $lineNo++
        # Commented lines are where install-specific examples are allowed to live.
        if ($line.TrimStart().StartsWith('#')) {
            continue
        }
        foreach ($match in [regex]::Matches($line, $refPattern)) {
            $activeRefs++
            if (-not $validSet.Contains($match.Value)) {
                Write-Host "::error file=$($file.FullName),line=$lineNo::'$($match.Value)' is not in entity-manifest.json - either the manifest is stale, or this is an install-specific (panel-label) entity that must only appear in commented examples / blueprint inputs"
                $failures++
            }
        }
    }
}

if ($checkedFiles -eq 0) {
    Write-Host "::error::no companion YAML files found under $companionDir"
    exit 1
}

if ($failures -gt 0) {
    Write-Host "::error::$failures unknown entity reference(s) across $checkedFiles files"
    exit 1
}

Write-Host "OK: $activeRefs active entity reference(s) across $checkedFiles files all match entity-manifest.json ($($validIds.Count) static entities)."
exit 0
