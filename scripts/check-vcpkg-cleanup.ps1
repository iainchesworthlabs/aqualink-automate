#!/usr/bin/env pwsh
#
# vcpkg disk-hygiene guard.
#
# vcpkg keeps `deps/vcpkg/buildtrees` and `deps/vcpkg/packages` after every port
# build. Nothing prunes them: _build.yml's self-hosted "Clean workspace" step does
# `rm -rf build install` and deliberately does not touch deps/vcpkg, so on a
# persistent runner they grow monotonically (measured ~6.2 GB, with OpenSSL's
# buildtree alone ~1.0 GB and boost-test next at 425 MB). That is what exhausted
# the 18 GB Linux / 16 GB Windows data volumes:
#
#   ld.bfd: final link failed: No space left on device          (Linux, at [723/726])
#   fatal error C1085: Cannot write compiler generated file ...  (MSVC)
#     : No space left on device
#
# The fix is VCPKG_INSTALL_OPTIONS with --clean-buildtrees-after-build and
# --clean-packages-after-build. It MUST be delivered as a preset cacheVariable,
# NOT as a `-D` on a command line:
#
#   `VCPKG_INSTALL_OPTIONS` is a CMake LIST, so its elements are separated by ';'.
#   Passing `-DVCPKG_INSTALL_OPTIONS=a;b` through lukka/run-cmake's
#   `configurePresetAdditionalArgs` fails, because that action invokes cmake via a
#   shell (`shell: true`) which splits on ';' — the macOS/Windows Configure died at
#   exit 127 with "/bin/sh: --clean-packages-after-build: command not found". That
#   is exactly how a previous attempt was reverted. A cacheVariable never reaches a
#   shell, so the ';' survives as a list separator (the same channel already carries
#   "CMAKE_CONFIGURATION_TYPES": "Debug;Release" without trouble).
#
# This guard therefore checks BOTH halves: the option is present via the safe
# channel, and nobody has re-introduced it via the unsafe one.
#
# Usage:  ./scripts/check-vcpkg-cleanup.ps1 [-Root <repo-root>]
# Exit:   0 = clean, 1 = violation(s) found.

param(
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

$requiredFlags = @('--clean-buildtrees-after-build', '--clean-packages-after-build')
$violations = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------
# 1. The option must be set on the `core` preset, which every CONCRETE preset
#    resolves to. Setting it lower down would silently miss presets.
# ---------------------------------------------------------------------------
$presetsPath = Join-Path $Root 'CMakePresets.json'
if (-not (Test-Path $presetsPath)) {
    Write-Error "CMakePresets.json not found at $presetsPath"
    exit 1
}
$presets = Get-Content $presetsPath -Raw | ConvertFrom-Json
$byName = @{}
foreach ($p in $presets.configurePresets) { $byName[$p.name] = $p }

$core = $byName['core']
if (-not $core) {
    $violations.Add("CMakePresets.json: no 'core' configure preset — this guard assumes it is the shared base.")
} else {
    $opts = $core.cacheVariables.VCPKG_INSTALL_OPTIONS
    if ([string]::IsNullOrWhiteSpace($opts)) {
        $violations.Add("CMakePresets.json: preset 'core' has no VCPKG_INSTALL_OPTIONS cacheVariable. vcpkg buildtrees/packages will accumulate on the persistent runners until a build dies with 'No space left on device'.")
    } else {
        foreach ($flag in $requiredFlags) {
            if ($opts -notlike "*$flag*") {
                $violations.Add("CMakePresets.json: preset 'core' VCPKG_INSTALL_OPTIONS is missing '$flag' (found: '$opts').")
            }
        }
        # The ';' is what makes it a CMake list. A space-separated value would be
        # passed to vcpkg as ONE malformed argument.
        if ($requiredFlags.Count -gt 1 -and $opts -notmatch ';') {
            $violations.Add("CMakePresets.json: preset 'core' VCPKG_INSTALL_OPTIONS must separate elements with ';' (CMake list syntax), not spaces. Found: '$opts'.")
        }
    }
}

# ---------------------------------------------------------------------------
# 2. Every CONCRETE preset must resolve to `core`, or it silently misses the
#    option — the failure mode this guard exists to prevent.
# ---------------------------------------------------------------------------
function Resolve-InheritsCore {
    param([string]$Name, [System.Collections.Generic.HashSet[string]]$Seen)
    if (-not $Seen) { $Seen = New-Object 'System.Collections.Generic.HashSet[string]' }
    if (-not $Seen.Add($Name)) { return $false }   # cycle guard
    if ($Name -eq 'core') { return $true }
    $p = $byName[$Name]
    if (-not $p -or -not $p.inherits) { return $false }
    foreach ($parent in @($p.inherits)) {
        if (Resolve-InheritsCore -Name $parent -Seen $Seen) { return $true }
    }
    return $false
}

foreach ($p in $presets.configurePresets) {
    if ($p.hidden) { continue }   # hidden mix-ins are combined WITH core, not instead of it
    if (-not (Resolve-InheritsCore -Name $p.name)) {
        $violations.Add("CMakePresets.json: concrete preset '$($p.name)' does not inherit 'core', so it will NOT get VCPKG_INSTALL_OPTIONS.")
    }
}

# ---------------------------------------------------------------------------
# 3. Nobody may re-introduce it through a shell-quoted command line. This is the
#    exact regression that broke macOS/Windows Configure at exit 127.
# ---------------------------------------------------------------------------
$workflowDir = Join-Path $Root '.github/workflows'
if (Test-Path $workflowDir) {
    foreach ($wf in Get-ChildItem $workflowDir -Filter *.yml -File) {
        $lineNo = 0
        foreach ($line in Get-Content $wf.FullName) {
            $lineNo++
            if ($line -match '^\s*#') { continue }   # a comment explaining the history is fine
            if ($line -match '-D\s*VCPKG_INSTALL_OPTIONS') {
                $violations.Add("$($wf.Name):${lineNo}: VCPKG_INSTALL_OPTIONS passed as a -D command-line argument. The ';' separator is split by the shell run-cmake invokes, which fails Configure at exit 127. Set it as a cacheVariable on the 'core' preset instead.")
            }
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "vcpkg disk-hygiene guard FAILED:`n" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  - $v" -ForegroundColor Red }
    Write-Host ''
    exit 1
}

Write-Host "vcpkg disk-hygiene guard passed: 'core' sets VCPKG_INSTALL_OPTIONS ($($requiredFlags -join ', ')), every concrete preset inherits it, and no workflow passes it via -D." -ForegroundColor Green
exit 0
