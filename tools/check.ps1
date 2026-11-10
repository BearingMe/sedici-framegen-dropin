<#
.SYNOPSIS
  One command for the developer checks (docs/dev/workflow.md). Prints one PASS/FAIL row per tool.

.DESCRIPTION
  -Quick  format check, warnings as errors, module layering.        (about 2 minutes, no GPU)
  -Full   Quick plus MSVC /analyze, clang-tidy, ASan and UBSan builds, and the CPU/GPU leak checks.
  -Only   run named steps only: format, warnings, layers, analyze, tidy, asan, ubsan, leaks.

  Every step builds in its own build-check-<step> directory (ignored by git), so the normal build/
  folder and the shipped DLL are never touched. Nothing is downloaded; the tools must already be
  installed (Visual Studio 2022 with C++, LLVM in C:\Program Files\LLVM). Vendored code under
  third_party/ is excluded from every tool.

  Exit code: 0 when every step passed, 1 otherwise.
#>
[CmdletBinding()]
param(
    [switch]$Quick,
    [switch]$Full,
    [string[]]$Only = @(),
    [switch]$KeepGoing = $true
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent $PSScriptRoot
Set-Location $Repo

if (-not $Quick -and -not $Full -and $Only.Count -eq 0) { $Quick = $true }
$steps = @()
if ($Only.Count -gt 0) { $steps = $Only }
elseif ($Full) { $steps = 'format', 'warnings', 'layers', 'analyze', 'tidy', 'asan', 'ubsan', 'leaks' }
else { $steps = 'format', 'warnings', 'layers' }

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $p = Join-Path $env:ProgramFiles "LLVM\bin\$name.exe"
    if (Test-Path $p) { return $p }
    return $null
}

# Import the MSVC x64 environment (cl, link, Ninja needs it) once.
function Import-VsEnvironment {
    if ($script:VsLoaded) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: Visual Studio 2022 with the C++ workload is required' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ x64 tools was not found' }
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $dump = cmd /c "`"$bat`" >nul 2>nul && set" 2>$null
    $ErrorActionPreference = $old
    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
    }
    $script:VsLoaded = $true
}

function Run([string]$exe, [string[]]$arguments, [string]$log) {
    # Merge stderr into the log without PowerShell turning it into errors.
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & $exe @arguments *>&1 | Out-File -FilePath $log -Encoding utf8
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return $code
}

$LogDir = Join-Path $Repo 'build-check-logs'
New-Item -ItemType Directory -Force $LogDir | Out-Null
$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$step, [bool]$ok, [string]$detail, [string]$log) {
    $results.Add([pscustomobject]@{ Step = $step; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Detail = $detail; Log = $log })
}

function Count-Matches([string]$log, [string]$pattern) {
    if (-not (Test-Path $log)) { return 0 }
    return @(Select-String -Path $log -Pattern $pattern).Count
}

# Configure and build one tooling configuration; returns the build log path and exit code.
function Build-Config([string]$name, [string[]]$configureArgs, [string]$config = 'Release') {
    $dir = Join-Path $Repo "build-check-$name"
    $log = Join-Path $LogDir "$name.log"
    $code = Run 'cmake' (@('-S', $Repo, '-B', $dir, '-DSEDICI_BUILD_TESTS=ON', '-DSEDICI_BUILD_FGCHECK=ON') + $configureArgs) "$log.configure"
    if ($code -ne 0) { Copy-Item "$log.configure" $log -Force; return @{ Dir = $dir; Log = $log; Code = $code } }
    $code = Run 'cmake' @('--build', $dir, '--config', $config, '--parallel') $log
    return @{ Dir = $dir; Log = $log; Code = $code }
}

foreach ($step in $steps) {
    Write-Host "== $step"
    try {
        switch ($step) {
            'format' {
                $cf = Find-Tool 'clang-format'
                if (-not $cf) { Add-Result $step $false 'clang-format not found' ''; break }
                $files = git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h'
                $log = Join-Path $LogDir 'format.log'
                $code = Run $cf (@('--style=file', '--dry-run', '--Werror') + $files) $log
                $n = Count-Matches $log 'error:'
                Add-Result $step ($code -eq 0) "$n formatting differences in $($files.Count) files" $log
            }
            'warnings' {
                $b = Build-Config 'warnings' @('-A', 'x64', '-DSEDICI_WERROR=ON')
                $n = Count-Matches $b.Log 'warning C|error C'
                Add-Result $step ($b.Code -eq 0) "$n compiler diagnostics at /W4 /WX" $b.Log
            }
            'layers' {
                $log = Join-Path $LogDir 'layers.log'
                $code = Run 'powershell.exe' @('-NoProfile', '-File', (Join-Path $Repo 'tools\check_layers.ps1'), '-Root', $Repo) $log
                Add-Result $step ($code -eq 0) 'module include layering' $log
            }
            'analyze' {
                $b = Build-Config 'analyze' @('-A', 'x64', '-DSEDICI_ANALYZE=ON', '-DSEDICI_WERROR=ON')
                $n = Count-Matches $b.Log 'warning C(6|28)\d+'
                Add-Result $step ($b.Code -eq 0) "$n /analyze findings" $b.Log
            }
            'tidy' {
                Import-VsEnvironment
                $ninja = Find-Tool 'ninja'
                if (-not $ninja) { Add-Result $step $false 'ninja not found' ''; break }
                $b = Build-Config 'tidy' @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSEDICI_TIDY=ON')
                $n = Count-Matches $b.Log '(warning|error):.*\[(bugprone|clang-analyzer|clang-diagnostic|concurrency|performance|cppcoreguidelines|misc|modernize)'
                Add-Result $step (($b.Code -eq 0) -and ($n -eq 0)) "$n clang-tidy findings" $b.Log
            }
            { $_ -in 'asan', 'ubsan' } {
                Import-VsEnvironment
                $clangcl = Find-Tool 'clang-cl'
                if (-not $clangcl) { Add-Result $step $false 'clang-cl not found' ''; break }
                $san = if ($step -eq 'asan') { 'address' } else { 'undefined' }
                $b = Build-Config $step @('-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$clangcl", "-DSEDICI_SANITIZE=$san")
                if ($b.Code -ne 0) { Add-Result $step $false 'sanitizer build failed' $b.Log; break }
                # Copy the sanitizer runtime next to the test binaries from the installed LLVM (no download).
                $rt = Get-ChildItem (Join-Path $env:ProgramFiles 'LLVM\lib\clang') -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($rt) { Copy-Item $rt.FullName (Join-Path $b.Dir 'tests') -Force }
                $ctestLog = Join-Path $LogDir "$step.ctest.log"
                # Tests without the gpu label only: they need no adapter, so the run is deterministic and the GPU stays free.
                # sedici_abi_exports is left out: the sanitizer runtime exports its own symbols from the sanitized DLL
                # (113 instead of 5), and the shipped DLL is never built with a sanitizer. The default build covers it.
                $code = Run 'ctest' @('--test-dir', $b.Dir, '-C', 'Release', '-j4', '--output-on-failure', '-LE', 'gpu', '-E', 'sedici_abi_exports') $ctestLog
                $n = Count-Matches $ctestLog 'ERROR: AddressSanitizer|runtime error:'
                Add-Result $step (($code -eq 0) -and ($n -eq 0)) "$n sanitizer reports" $ctestLog
            }
            'leaks' {
                # DXGI live-object report (GPU/COM objects still alive when a test host's main() returns), via
                # tests/common/leak_check.h. Exit code 4 = objects left. Debug build, so the CRT debug heap also lists
                # CPU blocks still allocated at exit (exit code 3). Runs the GPU tests, so keep the GPU idle.
                $b = Build-Config 'leaks' @('-A', 'x64', '-DSEDICI_CRTDBG=ON') 'Debug'
                if ($b.Code -ne 0) { Add-Result $step $false 'build failed' $b.Log; break }
                $ctestLog = Join-Path $LogDir 'leaks.ctest.log'
                $code = Run 'ctest' @('--test-dir', $b.Dir, '-C', 'Debug', '--output-on-failure', '-L', 'abi|fgcheck|gpu') $ctestLog
                $n = Count-Matches $ctestLog '^LEAK: (Live|the CRT)'
                Add-Result $step (($code -eq 0) -and ($n -eq 0)) "$n leaked DXGI objects or heap reports at exit" $ctestLog
            }
            default { Add-Result $step $false 'unknown step' '' }
        }
    }
    catch {
        Add-Result $step $false $_.Exception.Message ''
    }
}

Write-Host ''
$results | Format-Table Step, Result, Detail -AutoSize | Out-String | Write-Host
$failed = @($results | Where-Object Result -eq 'FAIL')
if ($failed.Count -gt 0) {
    Write-Host "Logs: $LogDir"
    exit 1
}
Write-Host 'All steps passed.'
exit 0
