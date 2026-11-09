# Checks tools/analyze_log.ps1 and tools/analyze_presents.ps1 against known inputs (P4.6/P4.7):
# the tracked QA-11 log excerpt (counts verified by grep when the test was written) and small
# fabricated inputs whose answers are known by construction.
param([string]$Repo = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
$ErrorActionPreference = "Stop"
$fail = 0
function Check($ok, $what) { if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what"; $script:fail++ } }

$excerpt = Join-Path $Repo "tests\qa\qa-11-window-changes\qa-11-sedici.log"
$r = & (Join-Path $Repo "tools\analyze_log.ps1") -Log $excerpt -Json | ConvertFrom-Json
Check ($r.prepareSlotRejections -eq 3) "analyze_log counts the 3 prepare-slot rejections of QA-11"
Check ($r.destroyRefusals -eq 1) "analyze_log counts the destroy refusal"
Check ($r.stalls -eq 20 -and $r.longStalls -eq 6) "analyze_log counts 20 stalls, 6 of a second or more"
Check ($r.contextDestroys -eq 5 -and $r.destroysNotOk -eq 1) "analyze_log counts 5 destroys, 1 not OK"
Check ($r.periodicLines -eq 33) "analyze_log reads 33 periodic statistics lines"

# Fabricated log: stable codes, a slow call, an overlay exception, and a rate that settles.
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sedici-analyze-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $log = Join-Path $tmp "sedici.log"
    $lines = @(
        '[0.000 ms] sedici: read x (once, on the first ffx call)',
        '[100.0 ms] ffxCreateContext #1 ctx=0x10 result=0 (OK) chain=0x20001 (FrameGenerationCreate)',
        '[500.0 ms] fg: rejected: prepare slot is still in flight [code=E_SLOT_BUSY] [op=dispatch ctx=0x10]',
        '[600.0 ms] fg: slow call op=configure ctx=0x10 took 120.5 ms',
        '[700.0 ms] sedici: overlay disabled: an exception was thrown while drawing',
        '[1000.0 ms] sedici: overlay present frames=300 real=292 generated=8 output_fps=66.00 real_fps=64.00 span_ms=4500.0',
        '[5000.0 ms] sedici: overlay present frames=300 real=200 generated=100 output_fps=70.00 real_fps=45.00 span_ms=4000.0',
        '[9000.0 ms] sedici: overlay present frames=300 real=150 generated=150 output_fps=74.00 real_fps=37.50 span_ms=4000.0',
        '[13000.0 ms] sedici: overlay present frames=300 real=150 generated=150 output_fps=74.00 real_fps=37.40 span_ms=4000.0',
        '[17000.0 ms] sedici: overlay present frames=300 real=150 generated=150 output_fps=74.00 real_fps=37.60 span_ms=4000.0',
        '[20000.0 ms] ffxDestroyContext #1 ctx=0x10 result=0 (OK) chain=none'
    )
    Set-Content -LiteralPath $log -Value $lines -Encoding ascii
    $f = & (Join-Path $Repo "tools\analyze_log.ps1") -Log $log -Json | ConvertFrom-Json
    Check ($f.rejectionsByCode[0].code -eq "E_SLOT_BUSY" -and $f.prepareSlotRejections -eq 1) "analyze_log groups a rejection by its stable code"
    Check ($f.slowCalls -eq 1) "analyze_log counts a slow call"
    Check ($f.overlayDisabled -eq 1) "analyze_log reports an overlay that disabled itself"
    Check ($f.settle.Count -eq 1 -and $f.settle[0].settledAfterMs -eq 8900) "analyze_log measures the settle time of the real rate (8.9 s)"

    # Present intervals: 30 fps steady for 10 s with one 2.5 s hitch.
    $csv = Join-Path $tmp "presents.csv"
    $rows = @("Application,TimeInSeconds,MsBetweenPresents")
    $t = 0.0
    for ($i = 0; $i -lt 300; ++$i) { $ms = 33.333; if ($i -eq 150) { $ms = 2500.0 }; $t += $ms / 1000.0; $rows += ("game.exe," + $t.ToString("F4", [cultureinfo]::InvariantCulture) + "," + $ms.ToString("F3", [cultureinfo]::InvariantCulture)) }
    Set-Content -LiteralPath $csv -Value $rows -Encoding ascii
    $p = & (Join-Path $Repo "tools\analyze_presents.ps1") -Csv $csv -TargetFps 30 -Json | ConvertFrom-Json
    Check ($p.frames -eq 300 -and $p.stallsOverOneSecond -eq 1) "analyze_presents counts 300 frames and one stall over a second"
    Check ([math]::Abs($p.percentWithinTolerance - 99.0) -lt 1.0) "analyze_presents: about 99 percent of intervals within tolerance of the target"
    Check ($p.p99Ms -ge 33.0 -and $p.maxMs -ge 2499.0) "analyze_presents reports the 99th percentile and the maximum"
}
finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($fail) { Write-Host "analyze_check: $fail failure(s)"; exit 1 }
Write-Host "analyze_check: ok"
exit 0
