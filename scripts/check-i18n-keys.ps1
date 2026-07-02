# i18n catalog guard (see docs/i18n.md).
#
# Checks, in order:
#   1. Completeness — every key referenced via t()/$t()/$tn()/labelKey:/roleKey:/
#      key-maps in the web assets must exist in the English catalog (en.js).
#   2. Parity — every non-English catalog must define exactly the same key set
#      as en.js (missing = untranslated string; extra = typo / dead key).
#   3. Placeholders — every {param} used in an English value must appear in the
#      corresponding translated value (and no new ones invented).
#
# Exit code 1 on any failure; unreferenced English keys are reported as
# informational only. Cross-platform (Windows + Linux CI: `pwsh`).
param([string]$Root = (Get-Location).Path)

$webRoot = Join-Path $Root 'assets/web'
$i18nDir = Join-Path $webRoot 'i18n'
$failures = 0

function Get-CatalogKeys([string]$file) {
    $src = Get-Content $file -Raw
    $map = [ordered]@{}
    foreach ($m in [regex]::Matches($src, "'([a-z_]+(?:\.[A-Za-z0-9_]+)+)'\s*:\s*'((?:[^'\\]|\\.)*)'")) {
        $map[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $map
}

$enFile = Join-Path $i18nDir 'en.js'
$en = Get-CatalogKeys $enFile
$catalogKeys = [System.Collections.Generic.HashSet[string]]::new([string[]]$en.Keys)

# ---- 1. Completeness against source references --------------------------------
# Exclusions test the path RELATIVE to the web root — the checkout directory
# itself may be called 'i18n', which would otherwise match every file.
$files = Get-ChildItem -Recurse $webRoot -File |
    Where-Object { $_.Extension -in '.js', '.html' } |
    Where-Object {
        $rel = $_.FullName.Substring($webRoot.Length + 1) -replace '\\', '/'
        $rel -notmatch '^(vendor|i18n)/'
    }

$used = [System.Collections.Generic.Dictionary[string, object]]::new()
function Add-Used([string]$key, [string]$file) {
    if (-not $used.ContainsKey($key)) { $used[$key] = [System.Collections.Generic.HashSet[string]]::new() }
    [void]$used[$key].Add($file)
}

foreach ($f in $files) {
    $src = Get-Content $f.FullName -Raw
    $rel = $f.FullName.Substring($webRoot.Length + 1) -replace '\\', '/'
    foreach ($m in [regex]::Matches($src, "(?:\`$t|\`$tn|\bt|AquaI18n\.t|\.tn)\(\s*'([a-z_]+(?:\.[A-Za-z0-9_]+)+)'")) {
        Add-Used $m.Groups[1].Value $rel
    }
    foreach ($m in [regex]::Matches($src, "(?:labelKey|roleKey):\s*'([a-z_]+(?:\.[A-Za-z0-9_]+)+)'")) {
        Add-Used $m.Groups[1].Value $rel
    }
    foreach ($m in [regex]::Matches($src, ":\s*'((?:common|status|alert|swg_health|chem|about)\.[A-Za-z0-9_]+)'")) {
        Add-Used $m.Groups[1].Value $rel
    }
    if ($src -match "'tier\.' \+ tier") { foreach ($k in 'tier.good', 'tier.okay', 'tier.bad') { Add-Used $k $rel } }
    if ($src -match "'sched\.act_' \+") { foreach ($k in 'sched.act_on', 'sched.act_off', 'sched.act_toggle') { Add-Used $k $rel } }
}

$missing = @()
foreach ($key in $used.Keys) {
    if ($key.EndsWith('_')) { continue }                    # dynamic key prefix ('ns.x_' + expr)
    if ($catalogKeys.Contains($key)) { continue }
    if ($catalogKeys.Contains("$key.other")) { continue }   # plural family (tn)
    $missing += "$key   (used in: $($used[$key] -join ', '))"
}

Write-Output "en catalog keys: $($catalogKeys.Count); referenced keys: $($used.Count)"
if ($missing.Count) {
    Write-Output "FAIL: keys referenced in code but MISSING from en.js ($($missing.Count)):"
    $missing | Sort-Object | ForEach-Object { Write-Output "  $_" }
    $failures++
} else {
    Write-Output "OK: no referenced key is missing from en.js."
}

# ---- 2 + 3. Parity + placeholder integrity for every other locale --------------
$phRe = [regex]'\{(\w+)\}'
foreach ($localeFile in Get-ChildItem $i18nDir -Filter *.js | Where-Object Name -ne 'en.js') {
    $code = [IO.Path]::GetFileNameWithoutExtension($localeFile.Name)
    $loc = Get-CatalogKeys $localeFile.FullName
    $locKeys = [System.Collections.Generic.HashSet[string]]::new([string[]]$loc.Keys)

    $absent = @($en.Keys | Where-Object { -not $locKeys.Contains($_) })
    $extra = @($loc.Keys | Where-Object { -not $catalogKeys.Contains($_) })
    $phBad = @()
    foreach ($key in $en.Keys) {
        if (-not $locKeys.Contains($key)) { continue }
        $want = @($phRe.Matches($en[$key]) | ForEach-Object { $_.Groups[1].Value }) | Sort-Object -Unique
        $have = @($phRe.Matches($loc[$key]) | ForEach-Object { $_.Groups[1].Value }) | Sort-Object -Unique
        if (($want -join ',') -ne ($have -join ',')) {
            $phBad += "$key   (en: {$($want -join '},{')}; ${code}: {$($have -join '},{')})"
        }
    }

    Write-Output "`nlocale '$code': $($locKeys.Count) keys"
    if ($absent.Count) {
        Write-Output "FAIL: keys missing from ${code}.js ($($absent.Count)):"
        $absent | Sort-Object | ForEach-Object { Write-Output "  $_" }
        $failures++
    }
    if ($extra.Count) {
        Write-Output "FAIL: keys in ${code}.js that do not exist in en.js ($($extra.Count)):"
        $extra | Sort-Object | ForEach-Object { Write-Output "  $_" }
        $failures++
    }
    if ($phBad.Count) {
        Write-Output "FAIL: placeholder mismatches in ${code}.js ($($phBad.Count)):"
        $phBad | Sort-Object | ForEach-Object { Write-Output "  $_" }
        $failures++
    }
    if (-not ($absent.Count -or $extra.Count -or $phBad.Count)) {
        Write-Output "OK: full parity with en.js, placeholders intact."
    }
}

# ---- Informational: English keys nothing references ----------------------------
$allSrc = ($files | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
$unused = @()
foreach ($key in $catalogKeys) {
    if ($used.ContainsKey($key)) { continue }
    $base = $key -replace '\.(zero|one|two|few|many|other)$', ''
    if ($base -ne $key -and $used.ContainsKey($base)) { continue }
    if ($allSrc.Contains("'$key'")) { continue }
    $unused += $key
}
if ($unused.Count) {
    Write-Output "`nUnreferenced en.js keys ($($unused.Count)) [informational]:"
    $unused | Sort-Object | ForEach-Object { Write-Output "  $_" }
}

exit ($failures -gt 0 ? 1 : 0)
