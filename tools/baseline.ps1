# Phase 3 baseline: print the behaviour record of a build in a form that does not
# depend on timing or on file paths. Run it on the phase2-baseline tag and on any
# later tree, then compare the two outputs.
#
#   pwsh tools/baseline.ps1 -Build build > after.txt
#   pwsh tools/baseline.ps1 -Build build -Against docs/dev/baseline.txt
#
# Records: the DLL exports, the CTest result per test name, the verdict lines and
# the quality lines of sedici_fgcheck, the verdict lines of sedici_regression.
# Timing values (ms, us) are dropped; PSNR/SSIM are kept.
param(
    [string]$Build = "build",
    [string]$Config = "Release",
    [string]$Against = "",
    [switch]$SkipCtest
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildDir = if ([IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $root $Build }
$out = New-Object System.Collections.Generic.List[string]

function Find-Dumpbin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vw) {
        $p = & $vw -latest -find "**\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
        if ($p) { return $p }
    }
    throw "dumpbin.exe not found"
}

$out.Add("== exports")
$dll = Join-Path $buildDir "$Config\amd_fidelityfx_framegeneration_dx12.dll"
$txt = & (Find-Dumpbin) /exports $dll
foreach ($l in $txt) {
    if ($l -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $out.Add($Matches[1]) }
}

if (-not $SkipCtest) {
    $out.Add("== ctest")
    Push-Location $buildDir
    try {
        $res = & ctest -C $Config -j4 2>&1
    } finally { Pop-Location }
    $rows = @()
    foreach ($l in $res) {
        if ($l -match 'Test\s+#\d+:\s+(\S+)\s+\.+\s*(\S.*?)\s+[\d.]+ sec') { $rows += "$($Matches[1]) $($Matches[2])" }
        elseif ($l -match 'Test\s+#\d+:\s+(\S+)\s+\.+\*{3}(\S.*?)\s+[\d.]+ sec') { $rows += "$($Matches[1]) $($Matches[2])" }
    }
    $rows | Sort-Object | ForEach-Object { $out.Add($_) }
    $out.Add("ctest-total " + $rows.Count)
}

function Strip-Timing([string]$l) {
    return ($l -replace '\s+stage1\s+[\d.]+ ms', '')
}

$fg = Join-Path $buildDir "tests\$Config\sedici_fgcheck.exe"
$out.Add("== fgcheck")
foreach ($l in (& $fg 2>&1)) {
    $s = "$l"
    if ($s -match '^(PASS|FAIL|XFAIL|SKIP)\b' -or $s -match 'psnr' -or $s -match 'preservation' -or $s -match '^display ') {
        $out.Add((Strip-Timing $s))
    }
}

$rg = Join-Path $buildDir "tests\$Config\sedici_regression.exe"
foreach ($mode in @("", "observer")) {
    $out.Add("== regression $mode")
    $args = @(); if ($mode) { $args = @($mode) }
    foreach ($l in (& $rg @args 2>&1)) {
        $s = "$l"
        if ($s -match '^(PASS|FAIL|XFAIL|SKIP)\b' -or $s -match 'checks, \d+ failed') { $out.Add($s) }
    }
}

if ($Against) {
    $ref = Get-Content -LiteralPath $Against
    $diff = Compare-Object $ref $out
    if ($diff) {
        $diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
        Write-Host "baseline: DIFFERENT ($(@($diff).Count) lines)"
        exit 1
    }
    Write-Host "baseline: identical ($($out.Count) lines)"
    exit 0
}
$out
