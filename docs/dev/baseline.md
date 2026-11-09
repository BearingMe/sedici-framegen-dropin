# Phase 3 baseline

The baseline is the behaviour of commit `e7481be`, tagged `phase2-baseline` (the commit that
closed P2.11). Phase 3 moves files and must not change any of it.

## What is recorded

`docs/dev/baseline.txt` is the output of `tools/baseline.ps1` on the tagged tree (265 lines):

- the five DLL exports, in order;
- the CTest result per test name (37 of 37 passed on an idle GPU);
- the PASS/FAIL lines and the PSNR/SSIM lines of `sedici_fgcheck` (timings dropped);
- the PASS/FAIL lines of `sedici_regression` and `sedici_regression observer`.

Timing values are not compared; they depend on the machine and on GPU load.

## Reproduce

```powershell
cmake -B build -A x64 -DSEDICI_BUILD_TESTS=ON -DSEDICI_BUILD_FGCHECK=ON
cmake --build build --config Release
.\tools\baseline.ps1 -Against docs\dev\baseline.txt     # prints "identical" or the differing lines
.\tools\check_moves.ps1                                  # source files vs the tag (run after git add -A)
```

## The two checks

| Check | Proves |
| --- | --- |
| `tools/baseline.ps1 -Against` | the built product behaves as before |
| `tools/check_moves.ps1` | every changed source file differs from the tag only by rename, `#include` lines, or path entries in `CMakeLists.txt` (`fg_` prefix and directory ignored) |

Both must pass after every slice. Added or deleted source files fail the move check.

## Final result (P3.6)

On 2026-11-09, after the last move, a fresh configure and build in an empty directory gave
`baseline: identical (265 lines)` against `docs/dev/baseline.txt` (exports, 37 of 37 CTest names,
fgcheck and regression verdicts), and `tools/check_moves.ps1` reported 65 renamed files and 2
edited only in include or source-list lines (`CMakeLists.txt`, `src/dllmain.cpp` counted
among the edited ones). Each slice commit recorded the same result. No in-game run was needed
(owner decision, 2026-11-09).
