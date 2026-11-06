param(
    [Parameter(Mandatory = $true)][string]$Label,
    [int]$Seconds = 30,
    [int]$WarmupSeconds = 45,
    [string]$ProcessName = 'Cyberpunk2077',
    [int]$WaitSeconds = 600,
    [string]$PresentMon,
    [string]$OutDir,
    # Extra PresentMon flags, space or comma separated, because `powershell -File`
    # flattens array parameters. When supplied they replace the default metric set,
    # e.g. -PresentMonArgs "--write_display_metadata -track_frame_type".
    [string]$PresentMonArgs = ''
)

# Waits for the game, captures one PresentMon CSV of the process, then copies
# sedici.log beside it. Never launches the game and never touches game config.

$ErrorActionPreference = 'Stop'

function Find-PresentMon {
    param([string]$Explicit)
    $candidates = @()
    if ($Explicit) { $candidates += $Explicit }
    if ($env:SEDICI_PRESENTMON) { $candidates += $env:SEDICI_PRESENTMON }

    foreach ($name in @('PresentMon.exe', 'PresentMon-x64.exe')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { $candidates += $command.Source }
    }

    $candidates += @(
        (Join-Path $env:ProgramFiles 'PresentMon\PresentMon.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'PresentMon\PresentMon.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\PresentMon\PresentMon.exe'),
        (Join-Path $env:ProgramFiles 'AMD\CNext\CNext\PresentMon-x64.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'AMD\CNext\CNext\PresentMon-x64.exe'),
        (Join-Path $env:ProgramFiles 'AMD\CNext\CNext\PresentMon.exe')
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    return $null
}

function Get-GameProcess {
    param([string]$Name, [int]$TimeoutSeconds)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $found = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
        if ($found.Count -gt 0) {
            return $found | Sort-Object StartTime | Select-Object -First 1
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

$presentMonExe = Find-PresentMon $PresentMon
if (-not $presentMonExe) {
    throw "PresentMon not found. Install PresentMon 2.x or pass -PresentMon <path to PresentMon.exe>."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'captures' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

Write-Host "PresentMon: $presentMonExe"
Write-Host "Waiting up to $WaitSeconds s for $ProcessName.exe ..."
$process = Get-GameProcess -Name $ProcessName -TimeoutSeconds $WaitSeconds
if (-not $process) {
    throw "Timed out after $WaitSeconds s waiting for $ProcessName.exe. Start the game and rerun."
}

Write-Host ("Capturing pid={0} start={1:yyyy-MM-dd HH:mm:ss} for {2} s" -f $process.Id, $process.StartTime, $Seconds)
if ($WarmupSeconds -gt 0) {
    Write-Host "Warm-up: $WarmupSeconds s to reach the test spot and start the camera pan."
    Write-Host "The $Seconds s capture window starts at $((Get-Date).AddSeconds($WarmupSeconds).ToString('HH:mm:ss'))."
    for ($remaining = $WarmupSeconds; $remaining -gt 0; $remaining--) {
        Write-Host -NoNewline "`r  capture starts in $remaining s   "
        Start-Sleep -Seconds 1
    }
    Write-Host "`r  capture window open          "
}
if (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
    throw "$ProcessName.exe exited during the warm-up; no capture started."
}


$date = Get-Date -Format 'yyyy-MM-dd'
$base = "$date-$Label"
$csv = Join-Path $OutDir "$base.csv"
if (Test-Path -LiteralPath $csv) {
    $base = "$date-$Label-" + (Get-Date -Format 'HHmmss')
    $csv = Join-Path $OutDir "$base.csv"
}

$sessionName = 'SediciCapture'
$arguments = @(
    '--process_id', "$($process.Id)",
    '--output_file', $csv,
    '--timed', "$Seconds",
    '--terminate_after_timed',
    '--no_console_stats',
    '--session_name', $sessionName,
    '--stop_existing_session'
)
$extra = $PresentMonArgs.Trim()
if ($extra -ieq 'default') {
    # PresentMon 2.x default metric set (adds DisplayedTime on newer builds).
} elseif ($extra.Length -gt 0) {
    $arguments += @($extra -split '[\s,]+' | Where-Object { $_ })
} else {
    # Stable column names: msBetweenPresents and msBetweenDisplayChange.
    $arguments += '--v1_metrics'
}

$stdoutPath = Join-Path $OutDir "$base.presentmon.stdout.txt"
$stderrPath = Join-Path $OutDir "$base.presentmon.stderr.txt"
$presentMonProcess = Start-Process -FilePath $presentMonExe -ArgumentList $arguments `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

if (Test-Path -LiteralPath $stderrPath) {
    $warnings = @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue |
        Where-Object { $_ -match '\S' })
    if ($warnings.Count -gt 0) {
        Write-Warning ("PresentMon: " + ($warnings -join ' '))
    }
    Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
}
Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue

if ($presentMonProcess.ExitCode -ne 0) {
    throw "PresentMon exited with code $($presentMonProcess.ExitCode). A capture session may need an elevated shell."
}
if (-not (Test-Path -LiteralPath $csv -PathType Leaf)) {
    throw "PresentMon produced no CSV at $csv."
}

$logSource = Join-Path (Split-Path -Parent $process.Path) 'sedici.log'
$logTarget = Join-Path $OutDir "$base.sedici.log"
if (Test-Path -LiteralPath $logSource -PathType Leaf) {
    Copy-Item -LiteralPath $logSource -Destination $logTarget -Force
    Write-Host "Copied sedici.log -> $logTarget"
} else {
    Write-Warning "No sedici.log next to the game executable ($logSource)."
}

$meta = Join-Path $OutDir "$base.meta.txt"
$logAsync = 'not seen in log'
$logPacing = 'not seen in log'
$logSource = 'not seen in log'
$logLevel = 'not seen in log'
$logTuning = 'not seen in log'
if (Test-Path -LiteralPath $logTarget -PathType Leaf) {
    $logText = Get-Content -LiteralPath $logTarget -Raw

    # Milestone 1 logs the effective configuration with provenance, so the meta
    # records what the process actually used rather than what the shell asked
    # for. "[default]", "[file]" and "[env]" say where the value came from.
    if ($logText -match 'pacing\.mode\s+= "([^"]+)" \[(\w+)\]') { $logPacing = "$($Matches[1]) ($($Matches[2]))" }
    if ($logText -match 'async\.mode\s+= "([^"]+)" \[(\w+)\]') { $logAsync = "$($Matches[1]) ($($Matches[2]))" }
    if ($logText -match 'log\.level\s+= (\d+) \[(\w+)\]') { $logLevel = "$($Matches[1]) ($($Matches[2]))" }
    if ($logText -match 'FramePacingTuning sent') {
        $logTuning = 'sent'
    } elseif ($logPacing -match '^hybrid') {
        $logTuning = 'not sent'
    } else {
        $logTuning = 'not requested'
    }
    if ($logText -match 'sedici: read ([^\r\n]+) \(once') { $logSource = $Matches[1] }
    elseif ($logText -match 'no sedici\.toml beside the DLL') { $logSource = 'no file, defaults' }
} else {
    $logAsync = 'no sedici.log'
    $logPacing = 'no sedici.log'
    $logSource = 'no sedici.log'
    $logLevel = 'no sedici.log'
    $logTuning = 'no sedici.log'
}
$lines = @(
    "label=$Label",
    "captured_at=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))",
    "process=$ProcessName pid=$($process.Id) start=$($process.StartTime.ToString('yyyy-MM-dd HH:mm:ss'))",
    "seconds=$Seconds",
    "presentmon=$presentMonExe",
    "exe=$($process.Path)",
    "sedici_pacing=$logPacing",
    "sedici_async=$logAsync",
    "sedici_log_level=$logLevel",
    "sedici_tuning=$logTuning",
    "sedici_config=$logSource",
    "capture_shell_async=$($env:SEDICI_ASYNC)",
    "capture_shell_pacing=$($env:SEDICI_PACING)",
    "sedici_async_user=$([Environment]::GetEnvironmentVariable('SEDICI_ASYNC','User'))",
    "sedici_pacing_user=$([Environment]::GetEnvironmentVariable('SEDICI_PACING','User'))",
    "sedici_log=$logTarget"
)
Set-Content -LiteralPath $meta -Value $lines -Encoding utf8

Write-Host "Wrote $csv"
Write-Host "Wrote $meta"
Write-Host ''
Write-Host "Next: python tools\analyze.py `"$csv`""
