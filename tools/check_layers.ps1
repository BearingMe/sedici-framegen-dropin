# Fails when a source file includes a header of a module at the same or a higher
# layer, which is how a dependency cycle starts (P4.5). The layers, lowest first:
#
#   diagnostics, gpu          0   log, stats, timing, fault injection; pure policies
#   settings, generator       1
#   presentation              2
#   runtime                   3
#   abi                       4
#   status, ui                5
#   src/ (dllmain.cpp)        6
#
# A module may include its own headers and anything in a strictly lower layer.
# docs/dev/layout.md carries the same table.
param([string]$Root = "")
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$rank = @{ diagnostics = 0; gpu = 0; settings = 1; generator = 1; presentation = 2; runtime = 3; abi = 4; status = 5; ui = 5; root = 6 }
$src = Join-Path $Root "src"
$bad = 0
$edges = @{}
foreach ($file in (Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $_.Extension -in '.h', '.cpp' })) {
    $rel = $file.FullName.Substring($src.Length + 1).Replace('\', '/')
    $from = if ($rel.Contains('/')) { $rel.Split('/')[0] } else { "root" }
    if (-not $rank.ContainsKey($from)) { Write-Host "FAIL unknown module '$from' for $rel"; $bad++; continue }
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        if ($line -match '^\s*#\s*include\s*"([^"]+)"') {
            $inc = $Matches[1]
            if (-not $inc.Contains('/')) { continue }
            $to = $inc.Split('/')[0]
            if (-not $rank.ContainsKey($to) -or $to -eq $from) { continue }
            $edges["$from -> $to"] = $true
            if ($rank[$to] -ge $rank[$from]) {
                Write-Host "FAIL $rel includes $inc ($from layer $($rank[$from]) -> $to layer $($rank[$to]))"
                $bad++
            }
        }
    }
}
if ($bad) { Write-Host "check_layers: $bad edge(s) go up or sideways"; exit 1 }
Write-Host "check_layers: ok ($($edges.Count) module edges, all downward)"
exit 0
