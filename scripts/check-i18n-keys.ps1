# Catalog completeness check (PowerShell port): every key referenced via
# t()/$t()/$tn()/labelKey:/roleKey:/key-maps in the web assets must exist in en.js.
param([string]$Root)

$webRoot = Join-Path $Root 'assets\web'
$catalogSrc = Get-Content (Join-Path $webRoot 'i18n\en.js') -Raw

# Catalog keys: lines of the form 'ns.key': '...'
$catalogKeys = [System.Collections.Generic.HashSet[string]]::new()
foreach ($m in [regex]::Matches($catalogSrc, "'([a-z_]+(?:\.[A-Za-z0-9_]+)+)'\s*:")) {
    [void]$catalogKeys.Add($m.Groups[1].Value)
}

# Exclusions must test the path RELATIVE to the web root — the worktree
# directory itself may be called 'i18n', which would otherwise match every file.
$files = Get-ChildItem -Recurse $webRoot -File |
    Where-Object { $_.Extension -in '.js', '.html' } |
    Where-Object {
        $rel = $_.FullName.Substring($webRoot.Length + 1)
        $rel -notmatch '^(vendor|i18n)\\'
    }

$used = [System.Collections.Generic.Dictionary[string, object]]::new()
function Add-Used([string]$key, [string]$file) {
    if (-not $used.ContainsKey($key)) { $used[$key] = [System.Collections.Generic.HashSet[string]]::new() }
    [void]$used[$key].Add($file)
}

foreach ($f in $files) {
    $src = Get-Content $f.FullName -Raw
    $rel = $f.FullName.Substring($webRoot.Length + 1)
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

# For the unused report, any literal occurrence of the key string anywhere in
# the sources counts (catches ternary-selected keys the t( regex missed).
$allSrc = ($files | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
$unused = @()
foreach ($key in $catalogKeys) {
    if ($used.ContainsKey($key)) { continue }
    $base = $key -replace '\.(zero|one|two|few|many|other)$', ''
    if ($base -ne $key -and $used.ContainsKey($base)) { continue }
    if ($allSrc.Contains("'$key'")) { continue }
    $unused += $key
}

Write-Output "catalog keys: $($catalogKeys.Count)"
Write-Output "referenced keys: $($used.Count)"
if ($missing.Count) {
    Write-Output "`nMISSING from catalog ($($missing.Count)):"
    $missing | Sort-Object | ForEach-Object { Write-Output "  $_" }
} else {
    Write-Output "`nNo missing keys."
}
if ($unused.Count) {
    Write-Output "`nUnreferenced catalog keys ($($unused.Count)) [informational]:"
    $unused | Sort-Object | ForEach-Object { Write-Output "  $_" }
}
exit ($missing.Count -gt 0 ? 1 : 0)
