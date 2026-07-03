#!/usr/bin/env pwsh
#
# Platform-isolation guard (docs/platform-isolation.md).
#
# Fails when an OPERATING-SYSTEM macro appears in a preprocessor directive in any
# shared, non-platform source file. In this codebase the OS is a CMake decision:
# OS-divergent code lives in src/core/platform/<os>/ and is selected by the
# if(WIN32)/if(LINUX)/if(APPLE) target_sources() blocks — never behind an #ifdef in
# shared code. A line such as `#elif !defined(__APPLE__)` should exist nowhere.
#
# COMPILER (_MSC_VER, __GNUC__, __clang__), ARCHITECTURE (__x86_64__, __i386__,
# __aarch64__), and BUILD-FEATURE (TRACY_ENABLE, ...) macros are a separate concern
# and are deliberately NOT flagged — see the "Allowed exceptions" section of the doc.
#
# Usage:  ./scripts/check-os-macros.ps1 [-Root <repo-root>]
# Exit:   0 = clean, 1 = violation(s) found.

param(
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

# OS tokens that must not gate shared code. Compiler/arch/feature macros are absent
# on purpose (see the doc's allowed-exceptions table).
$osTokens = @(
    '_WIN32', '_WIN64', 'WIN32',
    '__APPLE__', '__MACH__',
    '__linux__', '__unix__', '__unix',
    '__ANDROID__',
    '__FreeBSD__', '__NetBSD__', '__OpenBSD__', '__DragonFly__'
)
$tokenAlternation = ($osTokens | ForEach-Object { [Regex]::Escape($_) }) -join '|'

# A preprocessor conditional (#if / #ifdef / #ifndef / #elif) that references an OS token.
$pattern = '^\s*#\s*(if|ifdef|ifndef|elif)\b.*\b(' + $tokenAlternation + ')\b'

# Directories where OS macros are allowed / irrelevant (matches allowed, NOT scanned):
#   src/core/platform/**  — the OS-section dirs are where per-OS code legitimately lives.
#   cmake/**, deps/**, third_party/**, vcpkg_installed/**, build**/**  — build/toolchain/vendored.
$exemptRegex = '[\\/](platform[\\/](windows|posix|linux|macos)|deps|third_party|vcpkg_installed|out)[\\/]|[\\/]build[^\\/]*[\\/]|[\\/]cmake[\\/]'

$srcRoot = Join-Path $Root 'src'
if (-not (Test-Path $srcRoot)) {
    Write-Error "No src/ directory under '$Root'. Pass -Root <repo-root>."
    exit 2
}

$files = Get-ChildItem -Path $srcRoot -Recurse -File -Include '*.h', '*.hpp', '*.cpp', '*.cc', '*.cxx', '*.inl' |
    Where-Object { $_.FullName -notmatch $exemptRegex }

$violations = @()
foreach ($file in $files) {
    $matches = Select-String -Path $file.FullName -Pattern $pattern -AllMatches -CaseSensitive
    foreach ($m in $matches) {
        $violations += [pscustomobject]@{
            Path = [System.IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
            Line = $m.LineNumber
            Text = $m.Line.Trim()
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host ''
    Write-Host "Platform-isolation violation: OS macro(s) in shared code." -ForegroundColor Red
    Write-Host "OS divergence belongs in src/core/platform/<os>/ selected by CMake, not behind an #ifdef." -ForegroundColor Red
    Write-Host "See docs/platform-isolation.md.`n"
    foreach ($v in $violations) {
        Write-Host ("  {0}:{1}: {2}" -f $v.Path, $v.Line, $v.Text)
        # GitHub Actions annotation (no-op locally).
        Write-Host ("::error file={0},line={1}::OS macro in shared code — move it into src/core/platform/<os>/ (see docs/platform-isolation.md)" -f $v.Path, $v.Line)
    }
    Write-Host ("`n{0} violation(s) found." -f $violations.Count) -ForegroundColor Red
    exit 1
}

Write-Host "OK: no OS preprocessor macros in shared code ($($files.Count) files scanned)." -ForegroundColor Green
exit 0
