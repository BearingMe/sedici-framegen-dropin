# Phase 3 move checker: fails unless every difference between the working tree and
# the phase2-baseline tag is a rename, an #include line, a source-list or path entry
# in CMakeLists.txt, or a documentation/tooling file.
#
#   pwsh tools/check_moves.ps1                # compare against the tag
#   pwsh tools/check_moves.ps1 -Base <rev>
#
# Stage new and moved files first (git add -A): the checker reads the index plus
# the working tree through `git diff`, and untracked files are not seen.
param([string]$Base = "phase2-baseline")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

function Norm-Code([string[]]$lines, [bool]$cmake) {
    $o = New-Object System.Collections.Generic.List[string]
    foreach ($l in $lines) {
        $t = $l.TrimEnd("`r")
        if ($t -match '^\s*#\s*include\b') { continue }
        if ($cmake) {
            # paths of sources and tests: compare by base name, ignoring the fg_ prefix
            $t = [regex]::Replace($t, '(?:src|tests)/(?:[\w.]+/)*(?:fg_)?([\w.]+\.(?:cpp|h|ps1))', '$1')
            $t = [regex]::Replace($t, '"(?:\w+/)?imgui_config\.h"', '"imgui_config.h"')
        }
        $o.Add($t)
    }
    return ,$o.ToArray()
}

function Is-Exempt([string]$p) {
    return ($p -like "docs/*" -or $p -like "*.md" -or $p -like "tools/baseline.ps1" -or $p -like "tools/check_moves.ps1" -or $p -like ".gitignore")
}

$tmp = git rev-parse --verify --quiet "$Base^{commit}"
if (-not $tmp) { Write-Host "check_moves: base '$Base' not found"; exit 2 }

$rows = git diff --name-status -M60% $Base -- . ':!docs' ':!*.md' ':!tools/baseline.ps1' ':!tools/check_moves.ps1'
$bad = 0; $renamed = 0; $edited = 0
foreach ($r in $rows) {
    if (-not $r) { continue }
    $f = $r -split "`t"
    $st = $f[0].Substring(0, 1)
    switch ($st) {
        "R" { $old = $f[1]; $new = $f[2] }
        "M" { $old = $f[1]; $new = $f[1] }
        default { Write-Host "FAIL $st $($f[1])  (added or deleted files are not allowed)"; $bad++; continue }
    }
    $a = git show "${Base}:$old"
    $b = Get-Content -LiteralPath $new
    $cm = ($new -eq "CMakeLists.txt")
    $na = Norm-Code @($a) $cm
    $nb = Norm-Code @($b) $cm
    if (($na -join "`n") -ne ($nb -join "`n")) {
        Write-Host "FAIL changed beyond #include lines: $old -> $new"
        $d = Compare-Object $na $nb | Select-Object -First 6
        $d | ForEach-Object { Write-Host "   $($_.SideIndicator) $($_.InputObject)" }
        $bad++
    } elseif ($st -eq "R") { $renamed++ } else { $edited++ }
}
if ($bad) { Write-Host "check_moves: $bad file(s) differ beyond renames and include lines"; exit 1 }
Write-Host "check_moves: ok ($renamed renamed, $edited edited only in include/source lines)"
exit 0
