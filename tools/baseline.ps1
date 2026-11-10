# Behaviour baseline for the Rust build: record a run of the offline suite in a form
# that does not depend on timing, file paths or the machine, then compare two records.
#
#   pwsh tools/baseline.ps1 -Out after.txt
#   pwsh tools/baseline.ps1 -Against docs/dev/baseline.txt
#
# Builds the release DLL first when it is missing (`cargo build --release`), because
# the export and marker checks read it; `-SkipImage` or `-SkipTests` records only one
# half. `docs/dev/baseline.md` explains when to re-record.
#
# `-Out` writes UTF-8 without a BOM; `> file` works too, but Windows PowerShell 5.1
# prepends a BOM that the comparison ignores and a fresh checkout would not carry.
#
# It follows the procedure in docs/rust/test-parity.md (the Rust replacement for the
# C++ CTest baseline):
#
#   * one `test result:` summary per test binary, and the total;
#   * every test name with its verdict, from the same serial run that produced the
#     summaries (`cargo test -- --test-threads=1 --nocapture`, so a failure names its
#     test and a GPU test's figures stay beside it);
#   * the numeric lines the old baseline compared: the Stage 1 PSNR/SSIM and HUD
#     preservation figures, the 10-bit equivalence figures, and the present-queue
#     counts. Memory growth is recorded as "measured", not as a figure to compare;
#   * `SKIP …` and `NOTE …` lines, because a check the host could not run is a
#     difference from one that ran (the C++ suite's `SKIP_RETURN_CODE 77`);
#   * the built DLL's export table and its Sedici marker.
#
# Carriage returns are stripped before parsing and no line holds a host path, so the
# record compares across checkouts and line endings. Times (`finished in …`) are
# dropped. Never launches the game and never downloads anything; `cargo test` runs
# the GPU tests against the installed adapter.
param(
    [string]$Base = "",          # alias for -Against, the path to compare with
    [string]$Against = "",
    [string]$Out = "",           # write the record here (UTF-8, no BOM)
    [switch]$SkipTests,          # record only the image (exports and marker)
    [switch]$SkipImage           # record only the test run
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $Against) { $Against = $Base }
$record = New-Object System.Collections.Generic.List[string]

# --- the image: exports and marker -------------------------------------------------------------

function Get-PeExports {
    param([byte[]]$Bytes)
    $names = New-Object System.Collections.Generic.List[string]
    $u16 = { param($o) [BitConverter]::ToUInt16($Bytes, $o) }
    $u32 = { param($o) [BitConverter]::ToUInt32($Bytes, $o) }
    if ($Bytes.Length -lt 0x40) { return $names }
    $pe = [int](& $u32 0x3c)
    if ($pe + 24 -ge $Bytes.Length) { return $names }
    if ([Text.Encoding]::ASCII.GetString($Bytes, $pe, 4) -ne "PE`0`0") { return $names }
    $coff = $pe + 4
    $sections = [int](& $u16 ($coff + 2))
    $optionalSize = [int](& $u16 ($coff + 16))
    $optional = $coff + 20
    $magic = & $u16 $optional
    $directories = if ($magic -eq 0x20b) { $optional + 112 } else { $optional + 96 }
    if ($directories + 8 -gt $Bytes.Length) { return $names }
    $exportRva = [uint32](& $u32 $directories)
    $exportSize = [uint32](& $u32 ($directories + 4))
    if ($exportRva -eq 0 -or $exportSize -eq 0) { return $names }
    $sectionTable = $optional + $optionalSize
    $mapRva = {
        param([uint32]$Rva)
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $sectionTable + $i * 40
            if ($s + 24 -gt $Bytes.Length) { return $null }
            $virtualSize = & $u32 ($s + 8)
            $virtualAddress = & $u32 ($s + 12)
            $rawSize = & $u32 ($s + 16)
            $rawPointer = & $u32 ($s + 20)
            $span = [Math]::Max($virtualSize, $rawSize)
            if ($Rva -ge $virtualAddress -and $Rva -lt ($virtualAddress + $span)) {
                return [int]($rawPointer + ($Rva - $virtualAddress))
            }
        }
        return $null
    }
    $directory = & $mapRva $exportRva
    if ($null -eq $directory -or $directory + 40 -gt $Bytes.Length) { return $names }
    $nameCount = & $u32 ($directory + 24)
    $namesRva = & $u32 ($directory + 32)
    $namesBase = & $mapRva $namesRva
    if ($null -eq $namesBase) { return $names }
    for ($i = 0; $i -lt [int]$nameCount; $i++) {
        $entry = $namesBase + $i * 4
        if ($entry + 4 -gt $Bytes.Length) { break }
        $nameRva = & $u32 $entry
        $offset = & $mapRva $nameRva
        if ($null -eq $offset) { continue }
        $end = $offset
        while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) { $end++ }
        $names.Add([Text.Encoding]::ASCII.GetString($Bytes, $offset, $end - $offset))
    }
    return $names
}

if (-not $SkipImage) {
    $dll = Join-Path $root "target\release\amd_fidelityfx_framegeneration_dx12.dll"
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
        Write-Host "baseline: $dll is missing; running cargo build --release"
        & cargo build --release
        if ($LASTEXITCODE -ne 0) { throw "cargo build --release failed" }
    }
    $record.Add("== exports")
    $bytes = [IO.File]::ReadAllBytes($dll)
    foreach ($name in (Get-PeExports $bytes | Sort-Object)) { $record.Add($name) }
    $record.Add("== marker")
    $marker = "SEDICI_PROXY_V1"
    $present = [Text.Encoding]::ASCII.GetString($bytes).Contains($marker)
    $record.Add($(if ($present) { "SEDICI_PROXY_V1 present" } else { "SEDICI_PROXY_V1 MISSING" }))
}

# --- the test run -----------------------------------------------------------------------------

if (-not $SkipTests) {
    # Capture stderr from the native command without letting Windows PowerShell 5.1 turn each
    # stderr line into a terminating NativeCommandError; the figures are on stderr.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $raw = & cargo test -- --test-threads=1 --nocapture 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }

    $binary = "unknown"
    $binaries = New-Object System.Collections.Generic.List[string]
    $total = 0
    $verdicts = New-Object System.Collections.Generic.List[string]
    $figures = New-Object System.Collections.Generic.List[string]
    foreach ($line in $raw) {
        $text = $line.Replace("`r", "")
        # `-cmatch`: the harness writes `Running tests\…` (capital) and `running N tests` (lower);
        # only the first names a binary.
        if ($text -cmatch '^\s*Running (?:unittests )?(\S+)\s') {
            $binary = if ($Matches[1] -like "src*") { "lib" } else { [IO.Path]::GetFileNameWithoutExtension($Matches[1]) }
            continue
        }
        if ($text -cmatch '^\s*Doc-tests ') { $binary = "doc"; continue }
        if ($text -match '^test result: (ok|FAILED)\. (\d+) passed; (\d+) failed; (\d+) ignored') {
            $binaries.Add(("{0} {1} passed {2} failed {3} ignored" -f $binary, $Matches[2], $Matches[3], $Matches[4]))
            $total += [int]$Matches[2] + [int]$Matches[3] + [int]$Matches[4]
            continue
        }
        if ($text -cmatch '^test (?<name>.+?) \.\.\. (?<verdict>ok|FAILED|ignored)$') {
            $verdicts.Add(("{0} {1} {2}" -f $binary, $Matches['name'], $Matches['verdict']))
            continue
        }
        # A figure line may share its line with the harness prefix (`--nocapture` writes stderr
        # first); take everything after the last `... `.
        $figure = $text
        $index = $figure.LastIndexOf('... ')
        if ($index -ge 0) { $figure = $figure.Substring($index + 4) }
        $figure = [regex]::Replace($figure, '\s+stage1\s+[0-9.]+ ms', '')
        if ($figure -match 'psnr|hud preservation|memory over|video memory over|present queue counts') {
            # Absolute memory figures depend on allocator state; record that the check ran.
            $figure = [regex]::Replace($figure, 'grew by \d+ KiB', 'grew (size not compared)')
            $figure = [regex]::Replace($figure, '^test \S+ \.\.\. ', '')
            $figures.Add(($figure.Trim()))
        } elseif ($figure -match '^(SKIP|NOTE) ') {
            $figures.Add(($figure.Trim()))
        }
    }

    $record.Add("== binaries")
    foreach ($row in ($binaries | Sort-Object -Unique)) { $record.Add($row) }
    $record.Add("total $total tests")
    $record.Add("== verdicts")
    foreach ($row in ($verdicts | Sort-Object -Unique)) { $record.Add($row) }
    $record.Add("== figures")
    foreach ($row in ($figures | Sort-Object -Unique)) { $record.Add($row) }
    if ($code -ne 0) { $record.Add("exit $code") }
}

# --- compare ----------------------------------------------------------------------------------

if ($Against) {
    $ref = Get-Content -LiteralPath $Against
    $diff = Compare-Object $ref $record
    if ($diff) {
        $diff | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
        Write-Host "baseline: DIFFERENT ($(@($diff).Count) lines)"
        exit 1
    }
    Write-Host "baseline: identical ($($record.Count) lines)"
    exit 0
}
if ($Out) {
    $directory = Split-Path -Parent $Out
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory | Out-Null
    }
    [IO.File]::WriteAllLines((Join-Path $PWD $Out), $record, (New-Object Text.UTF8Encoding($false)))
    Write-Host "baseline: wrote $($record.Count) lines to $Out"
    exit 0
}
$record
