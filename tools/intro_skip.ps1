# Installs or removes the community "No Intro Videos" archive for Cyberpunk 2077
# (Nexus Mods, mod 533; file basegame_no_intro_videos.archive). It is not part of
# Sedici and not frame generation: the game plays its logo videos from its own
# archives, so the only way to skip them is an archive mod in archive\pc\mod.
#
#   .\tools\intro_skip.ps1 -Action Install -Archive C:\path\basegame_no_intro_videos.archive
#   .\tools\intro_skip.ps1 -Action Remove
#   .\tools\intro_skip.ps1 -Action Status
#
# Rules (the same spirit as deploy.ps1): never downloads anything (get the file from
# Nexus yourself), never launches the game, refuses while the game runs, refuses
# linked paths, never overwrites a different file of the same name, touches only
# archive\pc\mod\basegame_no_intro_videos.archive and its .sedici-intro-skip marker.
param(
    [Parameter(Mandatory = $true)][ValidateSet('Install', 'Remove', 'Status')][string]$Action,
    [string]$Archive,
    [string]$GamePath
)
$ErrorActionPreference = 'Stop'
$ModFile = 'basegame_no_intro_videos.archive'
$Marker = $ModFile + '.sedici-intro-skip'

function Resolve-GameRoot {
    param([string]$Explicit)
    $games = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'games.json') -Raw | ConvertFrom-Json
    $cfg = $games.cp2077
    $cands = @()
    if ($Explicit) { $cands = @($Explicit) } else {
        $roots = @()
        $steam = Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue
        if ($steam -and $steam.SteamPath) { $roots += $steam.SteamPath }
        $roots += [Environment]::ExpandEnvironmentVariables('%ProgramFiles(x86)%\Steam')
        $roots += [Environment]::ExpandEnvironmentVariables('%ProgramFiles%\Steam')
        foreach ($r in ($roots | Select-Object -Unique)) {
            $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf -PathType Leaf) {
                foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $roots += $m.Groups[1].Value.Replace('\\', '\')
                }
            }
        }
        foreach ($r in ($roots | Select-Object -Unique)) { $cands += Join-Path $r "steamapps\common\$($cfg.steamFolder)" }
        foreach ($h in $cfg.installDirHints) { $cands += [Environment]::ExpandEnvironmentVariables($h) }
    }
    foreach ($c in ($cands | Select-Object -Unique)) {
        # accept the install directory or its bin\x64 folder
        foreach ($root in @($c, (Split-Path -Parent (Split-Path -Parent $c)))) {
            if ($root -and (Test-Path -LiteralPath (Join-Path $root "$($cfg.binSubdir)\$($cfg.executable)") -PathType Leaf)) {
                return [IO.Path]::GetFullPath($root)
            }
        }
    }
    throw 'Cyberpunk 2077 not found. Supply -GamePath <install directory>.'
}

$root = Resolve-GameRoot $GamePath
$modDir = Join-Path $root 'archive\pc\mod'
$dest = Join-Path $modDir $ModFile
$mark = Join-Path $modDir $Marker

function Assert-NotLinked([string]$p) {
    if (Test-Path -LiteralPath $p) {
        if ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked path: $p" }
    }
}

if ($Action -ne 'Status' -and (Get-Process -Name 'Cyberpunk2077' -ErrorAction SilentlyContinue)) {
    throw 'Close Cyberpunk2077 first.'
}
Assert-NotLinked $modDir; Assert-NotLinked $dest; Assert-NotLinked $mark

switch ($Action) {
    'Status' {
        if (Test-Path -LiteralPath $dest -PathType Leaf) {
            $ours = Test-Path -LiteralPath $mark -PathType Leaf
            Write-Host "Installed: $dest ($(if ($ours) { 'by this script' } else { 'not placed by this script' }))"
        } else { Write-Host 'Not installed.' }
    }
    'Install' {
        if (-not $Archive) { throw 'Install needs -Archive <path to basegame_no_intro_videos.archive>.' }
        if ((Split-Path -Leaf $Archive) -ne $ModFile -or -not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
            throw "-Archive must be an existing file named $ModFile."
        }
        $srcHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
        if (Test-Path -LiteralPath $dest) {
            $h = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
            if ($h -eq $srcHash) { Write-Host 'Already installed with the same file.'; return }
            throw 'A different file with this name is already installed; refusing to overwrite it.'
        }
        New-Item -ItemType Directory -Force -Path $modDir | Out-Null
        Copy-Item -LiteralPath $Archive -Destination $dest
        if ((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -ne $srcHash) { Remove-Item -LiteralPath $dest; throw 'Copy hash mismatch; removed.' }
        Set-Content -LiteralPath $mark -Value $srcHash -Encoding ascii
        Write-Host "Installed $dest"
    }
    'Remove' {
        if (-not (Test-Path -LiteralPath $dest -PathType Leaf)) { Write-Host 'Nothing to remove.'; return }
        if (-not (Test-Path -LiteralPath $mark -PathType Leaf)) { throw 'The file was not placed by this script; remove it by hand.' }
        $want = (Get-Content -LiteralPath $mark -Raw).Trim()
        if ((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -ne $want) { throw 'The file changed since this script installed it; remove it by hand.' }
        Remove-Item -LiteralPath $dest; Remove-Item -LiteralPath $mark
        Write-Host "Removed $dest"
    }
}
