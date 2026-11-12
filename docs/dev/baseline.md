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

## Phase 4 re-records

`baseline.txt` is the Phase 2 record at the tag `phase2-baseline` for Phase 3. Phase 4 changes
behaviour on purpose, so the file is re-recorded after each milestone, and only after a comparison
showing that every previously passing line still passes and the differences are the additions that
the milestone made. P4.2 added the target-fps checks and `sedici_abi_target-auto` (38 CTest entries,
154 regression checks). P4.3 added `sedici_abi_window-replace-vsync` and the error-code checks (39 CTest entries,
156 regression checks). P4.4 added the `fast bar` scene to the fgcheck quality lines; the
other quality lines are unchanged to the printed precision. The P4.6 to P4.9 offline work added `sedici_analyzers`, the 10-bit equivalence lines, the cursor-scan checks and the New/ForHwnd routing checks (41 CTest entries, 163 regression checks, 303 lines); the comparison before the re-record showed only additions, and the regression host's three foreground checks present (K137). On 2026-11-12 (issue #1) the owner removed `multiplier = "adaptive"`: the four `adaptive schedule` lines went (two of them return as `2x schedule` lines), three `presenter loop` lines were added, and `present queue released with its last presenter`, added with K140 but never recorded, was recorded (41 CTest entries, 305 lines); every other line was unchanged. `git show phase2-baseline:docs/dev/baseline.txt` is the original.

## Final result (P3.6)

On 2026-11-09, after the last move, a fresh configure and build in an empty directory gave
`baseline: identical (265 lines)` against `docs/dev/baseline.txt` (exports, 37 of 37 CTest names,
fgcheck and regression verdicts), and `tools/check_moves.ps1` reported 65 renamed files and 2
edited only in include or source-list lines (`CMakeLists.txt`, `src/dllmain.cpp` counted
among the edited ones). Each slice commit recorded the same result. No in-game run was needed
(owner decision, 2026-11-09).
