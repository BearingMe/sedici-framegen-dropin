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

$dll = Join-Path $target.Bin $target.Config.targetDll
$backup = $dll + $target.Config.backupSuffix

if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
    throw "Original backup not found: $backup"
}

if (Test-Path -LiteralPath $dll) {
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'Target DLL path is not a file.' }
    if (-not (Test-SediciDll $dll)) { throw 'Installed DLL is not Sedici; leaving it and the backup untouched.' }
    Remove-Item -LiteralPath $dll -Force
}

if (Test-Path -LiteralPath $dll) {
    throw 'Target DLL still exists after removing Sedici; refusing to overwrite it.'
}

Move-Item -LiteralPath $backup -Destination $dll

$log = Join-Path $target.Bin 'sedici.log'
if (Test-Path -LiteralPath $log -PathType Leaf) { Remove-Item -LiteralPath $log -Force }

Write-Host "Restored $($target.Bin); original DLL is back in place."
