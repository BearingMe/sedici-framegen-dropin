# Turns a sedici.log (or a tests/qa excerpt) into the facts the in-game QA entries ask for,
# so a run can be judged without reading thousands of lines (P4.6, P4.7, P4.9).
#
#   tools\analyze_log.ps1 -Log <sedici.log> [-Json] [-StallMs 1000]
#
# Reports: run length; context creates and destroys and every non-OK result; every rejection
# with its stable error code (grouped); prepare stalls (all, and those of -StallMs or more);
# slow router calls; the overlay and Full-menu events; the periodic overlay statistics (real and
# output fps range) and, after every context create, how long the real rate took to settle
# (K123). It reads only; it never touches the log. Exit code 0 always: the verdicts are for the
# QA README, and each QA entry says which numbers must be zero.
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [switch]$Json,
    [double]$StallMs = 1000
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Log)) { Write-Host "analyze_log: no such file: $Log"; exit 2 }

$stamp = [regex]'^\[(?<t>[0-9.]+) ms\] '
$rejected = [regex]'fg: rejected: (?<why>.*?)(?: \[code=(?<code>E_[A-Z_]+)\])?(?: \[op=(?<op>\w+) ctx=(?<ctx>\S+)\])?\s*$'
$stall = [regex]'stall prepare interval=(?<ms>[0-9.]+) ms median=(?<med>[0-9.]+) ms'
$slow = [regex]'fg: slow call op=(?<op>\w+).*? took (?<ms>[0-9.]+) ms'
$create = [regex]'ffxCreateContext #\d+ ctx=\S+ result=(?<r>-?\d+) \((?<name>[A-Z_]+)\) chain=0x(?<type>[0-9a-f]+)'
$destroy = [regex]'ffxDestroyContext #\d+ ctx=\S+ result=(?<r>-?\d+) \((?<name>[A-Z_]+)\)'
$periodic = [regex]'overlay present frames=(?<n>\d+) real=(?<real>\d+) generated=(?<gen>\d+) output_fps=(?<out>[0-9.]+) real_fps=(?<rfps>[0-9.]+)'

$last = 0.0
$rejections = @(); $stalls = @(); $slowCalls = @(); $creates = @(); $destroys = @(); $events = @(); $periods = @()
foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Log))) {
    $m = $stamp.Match($line)
    if (-not $m.Success) { continue }
    $t = [double]$m.Groups['t'].Value
    if ($t -gt $last) { $last = $t }
    $text = $line.Substring($m.Length)
    if ($text -match 'fg: rejected:') {
        $r = $rejected.Match($text)
        if ($r.Success) {
            $code = if ($r.Groups['code'].Success) { $r.Groups['code'].Value } else { "(none)" }
            $rejections += [pscustomobject]@{ t = $t; why = $r.Groups['why'].Value; code = $code; op = $r.Groups['op'].Value }
        }
    } elseif ($stall.IsMatch($text)) {
        $s = $stall.Match($text)
        $stalls += [pscustomobject]@{ t = $t; ms = [double]$s.Groups['ms'].Value; median = [double]$s.Groups['med'].Value }
    } elseif ($slow.IsMatch($text)) {
        $s = $slow.Match($text)
        $slowCalls += [pscustomobject]@{ t = $t; op = $s.Groups['op'].Value; ms = [double]$s.Groups['ms'].Value }
    } elseif ($create.IsMatch($text)) {
        $c = $create.Match($text)
        $creates += [pscustomobject]@{ t = $t; result = [int]$c.Groups['r'].Value; name = $c.Groups['name'].Value; type = "0x" + $c.Groups['type'].Value }
    } elseif ($destroy.IsMatch($text)) {
        $d = $destroy.Match($text)
        $destroys += [pscustomobject]@{ t = $t; result = [int]$d.Groups['r'].Value; name = $d.Groups['name'].Value }
    } elseif ($periodic.IsMatch($text)) {
        $p = $periodic.Match($text)
        $periods += [pscustomobject]@{ t = $t; real = [int]$p.Groups['real'].Value; gen = [int]$p.Groups['gen'].Value
                                       out = [double]$p.Groups['out'].Value; rfps = [double]$p.Groups['rfps'].Value }
    } elseif ($text -match 'overlay disabled|exception|Full menu unavailable|interception (installed|unavailable)|overlay input hook|vrr presenter ready|falling back|device removed|DEVICE_REMOVED') {
        $events += [pscustomobject]@{ t = $t; text = $text.Trim() }
    }
}

# Settle time (K123): after each successful frame generation create, the first periodic line whose
# real fps is within 10 percent of the median of the later steady lines, until the next create.
$gens = $creates | Where-Object { $_.result -eq 0 -and $_.type -eq "0x20001" }
$settle = @()
for ($i = 0; $i -lt @($gens).Count; ++$i) {
    $from = $gens[$i].t
    $to = if ($i + 1 -lt @($gens).Count) { $gens[$i + 1].t } else { [double]::MaxValue }
    $window = @($periods | Where-Object { $_.t -ge $from -and $_.t -lt $to })
    if ($window.Count -lt 3) { continue }
    $tail = @($window | Select-Object -Last ([math]::Max(2, [int]($window.Count / 2))) | ForEach-Object { $_.rfps } | Sort-Object)
    $steady = $tail[[int]($tail.Count / 2)]
    $hit = $window | Where-Object { [math]::Abs($_.rfps - $steady) -le 0.10 * $steady } | Select-Object -First 1
    $settle += [pscustomobject]@{ createAtMs = $from; steadyRealFps = [math]::Round($steady, 1)
                                  settledAfterMs = if ($hit) { [math]::Round($hit.t - $from) } else { $null }
                                  firstRealFps = $window[0].rfps }
}

$longStalls = @($stalls | Where-Object { $_.ms -ge $StallMs })
$byCode = @($rejections | Group-Object code | ForEach-Object { [pscustomobject]@{ code = $_.Name; count = $_.Count; reasons = @($_.Group | ForEach-Object { $_.why } | Sort-Object -Unique) } })
$result = [pscustomobject]@{
    runSeconds       = [math]::Round($last / 1000.0, 1)
    contextCreates   = @($creates).Count
    createsNotOk     = @($creates | Where-Object { $_.result -ne 0 }).Count
    contextDestroys  = @($destroys).Count
    destroysNotOk    = @($destroys | Where-Object { $_.result -ne 0 }).Count
    rejections       = @($rejections).Count
    rejectionsByCode = $byCode
    prepareSlotRejections = @($rejections | Where-Object { $_.code -eq 'E_SLOT_BUSY' -or $_.why -match 'prepare slot' }).Count
    destroyRefusals  = @($rejections | Where-Object { $_.why -match 'destroy before GPU retirement' }).Count
    stalls           = @($stalls).Count
    longStalls       = $longStalls.Count
    longestStallMs   = if ($stalls) { [math]::Round(($stalls | Measure-Object ms -Maximum).Maximum, 1) } else { 0 }
    slowCalls        = @($slowCalls).Count
    overlayDisabled  = @($events | Where-Object { $_.text -match 'overlay disabled' }).Count
    fullMenuUnavailable = @($events | Where-Object { $_.text -match 'Full menu unavailable' }).Count
    periodicLines    = @($periods).Count
    realFpsMin       = if ($periods) { ($periods | Measure-Object rfps -Minimum).Minimum } else { $null }
    realFpsMax       = if ($periods) { ($periods | Measure-Object rfps -Maximum).Maximum } else { $null }
    outputFpsMax     = if ($periods) { ($periods | Measure-Object out -Maximum).Maximum } else { $null }
    settle           = $settle
}

if ($Json) { $result | ConvertTo-Json -Depth 5; exit 0 }

Write-Host ("Run: {0} s, {1} context creates ({2} not OK), {3} destroys ({4} not OK)" -f $result.runSeconds, $result.contextCreates, $result.createsNotOk, $result.contextDestroys, $result.destroysNotOk)
Write-Host ("Rejections: {0} ({1} prepare-slot, {2} destroy refusals)" -f $result.rejections, $result.prepareSlotRejections, $result.destroyRefusals)
foreach ($g in $byCode) { Write-Host ("  {0} x{1}: {2}" -f $g.code, $g.count, ($g.reasons -join "; ")) }
Write-Host ("Prepare stalls: {0} in all, {1} of {2:N0} ms or more, longest {3} ms" -f $result.stalls, $result.longStalls, $StallMs, $result.longestStallMs)
foreach ($s in $longStalls) { Write-Host ("  at {0,10:N1} s: {1,10:N1} ms (median {2:N1} ms)" -f ($s.t / 1000.0), $s.ms, $s.median) }
Write-Host ("Slow router calls (50 ms or more): {0}" -f $result.slowCalls)
foreach ($s in ($slowCalls | Select-Object -First 10)) { Write-Host ("  at {0,10:N1} s: {1} {2:N1} ms" -f ($s.t / 1000.0), $s.op, $s.ms) }
if ($periods) { Write-Host ("Real fps {0}..{1}, output fps up to {2} over {3} periodic lines" -f $result.realFpsMin, $result.realFpsMax, $result.outputFpsMax, $result.periodicLines) }
foreach ($s in $settle) {
    $took = if ($null -ne $s.settledAfterMs) { "{0:N1} s" -f ($s.settledAfterMs / 1000.0) } else { "did not settle" }
    Write-Host ("Real rate after the create at {0:N1} s: first line {1} fps, steady {2} fps, settled after {3}" -f ($s.createAtMs / 1000.0), $s.firstRealFps, $s.steadyRealFps, $took)
}
foreach ($e in $events) { Write-Host ("Event at {0,10:N1} s: {1}" -f ($e.t / 1000.0), $e.text) }
exit 0
