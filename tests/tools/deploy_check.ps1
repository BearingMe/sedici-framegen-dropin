# Offline check of tools/deploy.ps1 and tools/restore.ps1 against the deploy-safety
# rules in AGENTS.md. Builds a throwaway fake repository and fake game folder under
# the system temp directory; never touches a real game, never launches one.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/tools/deploy_check.ps1 `
#       -Dll target\release\amd_fidelityfx_framegeneration_dx12.dll -Repo .
#
# The fake repository puts the DLL at target\release, the path `cargo build --release`
# produces and tools/deploy.ps1 reads.
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$Repo
)

$ErrorActionPreference = 'Stop'
$script:failures = 0

function Check {
    param([bool]$Condition, [string]$Name)
    if ($Condition) { Write-Host "PASS $Name" } else { Write-Host "FAIL $Name"; $script:failures++ }
}

function Hash {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Run-Tool {
    param([string]$Script, [string[]]$Arguments)
    # The tools are expected to fail with a message on stderr. Under 'Stop', Windows
    # PowerShell 5.1 turns that stderr line into a terminating error of this script.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Script @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    return [pscustomobject]@{ Code = $code; Output = ($out | Out-String) }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("sedici-deploy-check-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $tools = Join-Path $work 'tools'
    $buildRelease = Join-Path $work 'target\release'
    $bin = Join-Path $work 'game\bin'
    New-Item -ItemType Directory -Path $tools, $buildRelease, $bin | Out-Null

    Copy-Item -LiteralPath (Join-Path $Repo 'tools\deploy.ps1') -Destination $tools
    Copy-Item -LiteralPath (Join-Path $Repo 'tools\restore.ps1') -Destination $tools
    Copy-Item -LiteralPath (Join-Path $Repo 'sedici.toml') -Destination $work
    $games = @{
        t = @{
            name = 'Test'; steamFolder = 'x'; installDirHints = @(); binSubdir = ''
            executable = 'SediciTestGame.exe'; targetDll = 'amd_fidelityfx_framegeneration_dx12.dll'
            backupSuffix = '.orig'; loaderDll = 'amd_fidelityfx_loader_dx12.dll'
            upscalerDll = 'amd_fidelityfx_upscaler_dx12.dll'
        }
    }
    $games | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $tools 'games.json') -Encoding UTF8

    $built = Join-Path $buildRelease 'amd_fidelityfx_framegeneration_dx12.dll'
    Copy-Item -LiteralPath $Dll -Destination $built
    $exe = Join-Path $bin 'SediciTestGame.exe'
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\cmd.exe') -Destination $exe

    $target = Join-Path $bin 'amd_fidelityfx_framegeneration_dx12.dll'
    $backup = $target + '.orig'
    $loader = Join-Path $bin 'amd_fidelityfx_loader_dx12.dll'
    $upscaler = Join-Path $bin 'amd_fidelityfx_upscaler_dx12.dll'
    $config = Join-Path $bin 'sedici.toml'
    $log = Join-Path $bin 'sedici.log'
    $deploy = Join-Path $tools 'deploy.ps1'
    $restore = Join-Path $tools 'restore.ps1'

    $original = [Text.Encoding]::ASCII.GetBytes('ORIGINAL-AMD-FRAME-GENERATION-DLL-BYTES')
    [IO.File]::WriteAllBytes($target, $original)
    [IO.File]::WriteAllBytes($loader, [Text.Encoding]::ASCII.GetBytes('loader'))
    [IO.File]::WriteAllBytes($upscaler, [Text.Encoding]::ASCII.GetBytes('upscaler'))
    $originalHash = Hash $target
    $loaderHash = Hash $loader
    $upscalerHash = Hash $upscaler
    $builtHash = Hash $built

    $toolArgs = @('-Game', 't', '-GamePath', $bin)

    # 1. First deploy.
    $r = Run-Tool $deploy $toolArgs
    Check ($r.Code -eq 0) 'deploy succeeds on an untouched install'
    Check ((Hash $backup) -eq $originalHash) 'the original DLL is preserved byte for byte as .orig'
    Check ((Hash $target) -eq $builtHash) 'the Sedici DLL is in place with the built hash'
    Check (Test-Path -LiteralPath $config) 'the default sedici.toml is written'
    Check (((Hash $loader) -eq $loaderHash) -and ((Hash $upscaler) -eq $upscalerHash)) 'the loader and upscaler DLLs are untouched'

    # 2. Redeploy keeps the user's configuration and never rewrites .orig.
    Set-Content -LiteralPath $config -Value '# my settings' -Encoding ASCII
    $mine = Hash $config
    $r = Run-Tool $deploy $toolArgs
    Check ($r.Code -eq 0) 'deploy is repeatable'
    Check ((Hash $config) -eq $mine) 'an existing sedici.toml is never overwritten'
    Check ((Hash $backup) -eq $originalHash) 'redeploying never overwrites an existing .orig'

    # 3. Both tools refuse while the game is running.
    $proc = Start-Process -FilePath $exe -ArgumentList '/c', 'ping -n 40 127.0.0.1 >nul' -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 500
        $before = Hash $target
        $r1 = Run-Tool $deploy $toolArgs
        $r2 = Run-Tool $restore $toolArgs
        Check (($r1.Code -ne 0) -and ($r2.Code -ne 0) -and ((Hash $target) -eq $before) -and (Test-Path -LiteralPath $backup)) `
            'deploy and restore refuse while the game process is running'
    } finally {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        $proc.WaitForExit(5000) | Out-Null
    }

    # 4. A foreign DLL at the target is never overwritten or removed.
    $foreign = [Text.Encoding]::ASCII.GetBytes('SOMEONE-ELSES-DLL')
    [IO.File]::WriteAllBytes($target, $foreign)
    $foreignHash = Hash $target
    $r1 = Run-Tool $deploy $toolArgs
    $r2 = Run-Tool $restore $toolArgs
    Check (($r1.Code -ne 0) -and ($r2.Code -ne 0) -and ((Hash $target) -eq $foreignHash) -and ((Hash $backup) -eq $originalHash)) `
        'a non-Sedici DLL is left alone by deploy and restore, and .orig stays'
    Copy-Item -LiteralPath $built -Destination $target -Force

    # 5. Restore puts everything back and removes only Sedici's own files.
    Set-Content -LiteralPath $log -Value 'log' -Encoding ASCII
    $r = Run-Tool $restore $toolArgs
    Check ($r.Code -eq 0) 'restore succeeds'
    Check ((Hash $target) -eq $originalHash) 'restore puts the original DLL back byte for byte'
    Check (-not (Test-Path -LiteralPath $backup)) 'restore consumes .orig'
    Check ((-not (Test-Path -LiteralPath $config)) -and (-not (Test-Path -LiteralPath $log))) 'restore removes sedici.toml and sedici.log'
    Check (((Hash $loader) -eq $loaderHash) -and ((Hash $upscaler) -eq $upscalerHash)) 'restore leaves the loader and upscaler DLLs untouched'
    $r = Run-Tool $restore $toolArgs
    Check (($r.Code -ne 0) -and ((Hash $target) -eq $originalHash)) 'restore with no .orig refuses and changes nothing'

    # 6. Sedici installed but its backup missing: refuse to deploy.
    $r = Run-Tool $deploy $toolArgs
    Remove-Item -LiteralPath $backup -Force
    $installed = Hash $target
    $r = Run-Tool $deploy $toolArgs
    Check (($r.Code -ne 0) -and ((Hash $target) -eq $installed)) 'deploy refuses when Sedici is installed but .orig is missing'
    Copy-Item -LiteralPath $built -Destination $target -Force
    [IO.File]::WriteAllBytes($backup, $original)
    $r = Run-Tool $restore $toolArgs
    Check ($r.Code -eq 0 -and (Hash $target) -eq $originalHash) 'restore recovers once .orig is back'

    # 7. A release DLL without the marker is never deployed.
    [IO.File]::WriteAllBytes($built, [Text.Encoding]::ASCII.GetBytes('not a sedici build'))
    $r = Run-Tool $deploy $toolArgs
    Check (($r.Code -ne 0) -and ((Hash $target) -eq $originalHash) -and (-not (Test-Path -LiteralPath $backup))) `
        'a release DLL without the Sedici marker is refused before anything is moved'
    Copy-Item -LiteralPath $Dll -Destination $built -Force

    # 8. Unknown game id and an empty install path fail cleanly.
    $r = Run-Tool $deploy @('-Game', 'nope', '-GamePath', $bin)
    Check ($r.Code -ne 0) 'an unknown game id is rejected'
    $r = Run-Tool $deploy @('-Game', 't', '-GamePath', (Join-Path $work 'missing'))
    Check ($r.Code -ne 0) 'a missing install path is rejected'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures -ne 0) {
    Write-Host "FAIL deploy and restore: $($script:failures) check(s) failed"
    exit 1
}
Write-Host 'PASS deploy and restore safety rules'
exit 0
