param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$GamePath
)

$ErrorActionPreference = 'Stop'
$script:ToolsRoot = $PSScriptRoot

function Get-GameEntry {
    param([string]$Id)
    $games = Get-Content -LiteralPath (Join-Path $script:ToolsRoot 'games.json') -Raw | ConvertFrom-Json
    $entry = $games.PSObject.Properties[$Id]
    if (-not $entry) { throw "Unknown game '$Id'. See tools/games.json." }
    return $entry.Value
}

function Test-BinPath {
    param([string]$Path, $Config)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    return (Test-Path -LiteralPath (Join-Path $Path $Config.executable) -PathType Leaf)
}

function Resolve-GameBin {
    param([string]$Id, [string]$ExplicitPath)
    $config = Get-GameEntry $Id
    $candidates = @()

    if ($ExplicitPath) {
        $candidates = @($ExplicitPath)
    } else {
        $steamRoots = @()
        $steam = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
        if ($steam -and $steam.SteamPath) { $steamRoots += $steam.SteamPath }
        $steamRoots += [Environment]::ExpandEnvironmentVariables('%ProgramFiles(x86)%\Steam')
        $steamRoots += [Environment]::ExpandEnvironmentVariables('%ProgramFiles%\Steam')

        foreach ($steamRoot in ($steamRoots | Select-Object -Unique)) {
            $libraries = Join-Path $steamRoot 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $libraries -PathType Leaf) {
                $text = Get-Content -LiteralPath $libraries -Raw
                foreach ($match in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                    $steamRoots += $match.Groups[1].Value.Replace('\\', '\')
                }
            }
        }

        foreach ($steamRoot in ($steamRoots | Select-Object -Unique)) {
            $candidates += Join-Path $steamRoot "steamapps\common\$($config.steamFolder)"
        }
        foreach ($hint in $config.installDirHints) {
            $candidates += [Environment]::ExpandEnvironmentVariables($hint)
        }
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-BinPath $candidate $config) {
            return [pscustomobject]@{ Config = $config; Bin = [IO.Path]::GetFullPath($candidate) }
        }
        $bin = Join-Path $candidate $config.binSubdir
        if (Test-BinPath $bin $config) {
            return [pscustomobject]@{ Config = $config; Bin = [IO.Path]::GetFullPath($bin) }
        }
    }

    throw "Installation for '$Id' not found. Supply -GamePath <install-directory or bin\x64>."
}

function Assert-GameStopped {
    param($Config, [string]$Bin)
    $processName = [IO.Path]::GetFileNameWithoutExtension($Config.executable)
    if (Get-Process -Name $processName -ErrorAction SilentlyContinue) {
        throw "Close $processName before deploying or restoring."
    }
    foreach ($path in @((Join-Path $Bin $Config.targetDll), (Join-Path $Bin ($Config.targetDll + $Config.backupSuffix)))) {
        if (Test-Path -LiteralPath $path) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing linked deployment path: $path"
            }
        }
    }
}

function Test-SediciDll {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path)).Contains('SEDICI_PROXY_V1')
}

$target = Resolve-GameBin $Game $GamePath
Assert-GameStopped $target.Config $target.Bin

$repoRoot = Split-Path -Parent $script:ToolsRoot
# `cargo build --release` writes the DLL to target\release (the crate is the DLL name).
$sourceDll = Join-Path $repoRoot 'target\release\amd_fidelityfx_framegeneration_dx12.dll'
if (-not (Test-Path -LiteralPath $sourceDll -PathType Leaf)) {
    throw 'Release Sedici DLL is missing; run `cargo build --release` first (target\release\amd_fidelityfx_framegeneration_dx12.dll).'
}
if (-not (Test-SediciDll $sourceDll)) {
    throw 'Release DLL does not contain the Sedici marker.'
}

$dll = Join-Path $target.Bin $target.Config.targetDll
$backup = $dll + $target.Config.backupSuffix

if (Test-Path -LiteralPath $backup) {
    if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) { throw 'Backup path is not a file.' }
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'Backup exists but the target DLL is missing.' }
    if (-not (Test-SediciDll $dll)) { throw 'Refusing to overwrite a non-Sedici DLL while .orig exists.' }
} else {
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'Original frame-generation DLL not found; cannot make a backup.' }
    if (Test-SediciDll $dll) { throw 'Sedici is installed but the .orig backup is missing; refusing to deploy.' }
    Move-Item -LiteralPath $dll -Destination $backup
}

Copy-Item -LiteralPath $sourceDll -Destination $dll -Force
$sourceHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash
$destHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
if ($sourceHash -ne $destHash) { throw 'Deployed DLL hash mismatch.' }

# Ship the default configuration beside the DLL, but never overwrite a copy that
# is already there: that file is the user's, and it is the documented way to
# change Sedici's behaviour without touching the environment.
$sourceConfig = Join-Path $repoRoot 'sedici.toml'
$destConfig = Join-Path $target.Bin 'sedici.toml'
if (-not (Test-Path -LiteralPath $sourceConfig -PathType Leaf)) {
    throw 'sedici.toml is missing from the repository root.'
}
if (Test-Path -LiteralPath $destConfig) {
    Write-Host "Kept existing $destConfig (not overwritten)."
} else {
    Copy-Item -LiteralPath $sourceConfig -Destination $destConfig
    Write-Host "Wrote default configuration to $destConfig."
}

Write-Host "Deployed Sedici to $($target.Bin); original preserved as .orig."
