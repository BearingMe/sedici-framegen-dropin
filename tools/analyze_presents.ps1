# Judges a present-interval capture (PresentMon, CapFrameX or any CSV with a frame time column)
# against what QA-04, QA-05 and QA-10 ask for, so the owner does not have to read the CSV (P4.6).
#
#   tools\analyze_presents.ps1 -Csv <capture.csv> [-TargetFps 60] [-TolerancePercent 10]
#                              [-StallMs 1000] [-Column MsBetweenPresents] [-Json]
#
# The frame time column is found by name (MsBetweenPresents, msBetweenPresents, FrameTime,
# frametime, MsBetweenDisplayChange) unless -Column is given. -TargetFps is the output rate you
# expect (the cadence criterion of QA-04); without it the median interval is the target.
# Reports frames, duration, average fps, interval percentiles, the 1 percent low fps, stalls of
# -StallMs or more, and the share of intervals within the tolerance of the target.
param(
    [Parameter(Mandatory = $true)][string]$Csv,
    [double]$TargetFps = 0,
    [double]$TolerancePercent = 10,
    [double]$StallMs = 1000,
    [string]$Column = "",
    [switch]$Json
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Csv)) { Write-Host "analyze_presents: no such file: $Csv"; exit 2 }
$rows = Import-Csv -LiteralPath $Csv
if (-not $rows) { Write-Host "analyze_presents: the file has no rows"; exit 2 }
$names = $rows[0].PSObject.Properties.Name
if (-not $Column) {
    foreach ($candidate in "MsBetweenPresents", "msBetweenPresents", "FrameTime", "frametime", "MsBetweenDisplayChange", "msBetweenDisplayChange") {
        if ($names -contains $candidate) { $Column = $candidate; break }
    }
}
if (-not $Column -or -not ($names -contains $Column)) {
    Write-Host ("analyze_presents: no frame time column found; columns are: " + ($names -join ", ")); exit 2
}
$ci = [System.Globalization.CultureInfo]::InvariantCulture
$ms = New-Object System.Collections.Generic.List[double]
foreach ($row in $rows) {
    $v = 0.0
    if ([double]::TryParse([string]$row.$Column, [System.Globalization.NumberStyles]::Float, $ci, [ref]$v) -and $v -gt 0) { $ms.Add($v) }
}
if ($ms.Count -lt 10) { Write-Host "analyze_presents: fewer than 10 usable intervals"; exit 2 }
$sorted = $ms.ToArray(); [Array]::Sort($sorted)
function Pct([double[]]$s, [double]$p) { $s[[math]::Min($s.Length - 1, [int][math]::Floor($p / 100.0 * $s.Length))] }
$sum = ($ms | Measure-Object -Sum).Sum
$median = Pct $sorted 50
$target = if ($TargetFps -gt 0) { 1000.0 / $TargetFps } else { $median }
$low = [math]::Max(1, [int][math]::Ceiling($sorted.Length * 0.01))
$worst = $sorted[($sorted.Length - $low)..($sorted.Length - 1)]
$within = ($ms | Where-Object { [math]::Abs($_ - $target) -le $target * $TolerancePercent / 100.0 }).Count
$result = [pscustomobject]@{
    column = $Column
    frames = $ms.Count
    durationSeconds = [math]::Round($sum / 1000.0, 1)
    averageFps = [math]::Round(1000.0 * $ms.Count / $sum, 2)
    targetMs = [math]::Round($target, 3)
    p50Ms = [math]::Round($median, 3)
    p95Ms = [math]::Round((Pct $sorted 95), 3)
    p99Ms = [math]::Round((Pct $sorted 99), 3)
    p999Ms = [math]::Round((Pct $sorted 99.9), 3)
    maxMs = [math]::Round($sorted[-1], 3)
    onePercentLowFps = [math]::Round(1000.0 / (($worst | Measure-Object -Average).Average), 2)
    stallsOverOneSecond = ($ms | Where-Object { $_ -ge $StallMs }).Count
    percentWithinTolerance = [math]::Round(100.0 * $within / $ms.Count, 2)
    tolerancePercent = $TolerancePercent
}
if ($Json) { $result | ConvertTo-Json; exit 0 }
Write-Host ("{0} frames over {1} s, average {2} fps (column {3})" -f $result.frames, $result.durationSeconds, $result.averageFps, $Column)
Write-Host ("Interval: median {0} ms, 95th {1}, 99th {2}, 99.9th {3}, max {4}; 1 percent low {5} fps" -f $result.p50Ms, $result.p95Ms, $result.p99Ms, $result.p999Ms, $result.maxMs, $result.onePercentLowFps)
Write-Host ("Stalls of {0:N0} ms or more: {1}" -f $StallMs, $result.stallsOverOneSecond)
Write-Host ("Cadence: {0}% of intervals within {1}% of the target {2} ms ({3} fps)" -f $result.percentWithinTolerance, $TolerancePercent, $result.targetMs, [math]::Round(1000.0 / $target, 1))
exit 0
