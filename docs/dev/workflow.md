# Workflow

## Build and test

```powershell
cmake -B build -A x64 -DSEDICI_BUILD_TESTS=ON -DSEDICI_BUILD_FGCHECK=ON
cmake --build build --config Release
cd build; ctest -C Release -j4      # 37 entries; GPU tests run serially, keep the GPU idle
dumpbin /exports build\Release\amd_fidelityfx_framegeneration_dx12.dll   # exactly five exports
```

Never launch the game, never download SDKs or DLLs (see `AGENTS.md`).

## Adding or moving a file

1. Put the file in the module that owns it ([layout.md](layout.md)). Use `git mv` when moving.
2. Include with a path relative to `src/`: `#include "settings/config.h"`. No `../`.
3. Add it to the source list in `CMakeLists.txt`. Tests keep their CTest names.
4. Update the layout section of `AGENTS.md` and [moves.md](moves.md) in the same commit.

## Behaviour-preserving changes (Phase 3 rules)

A reorganisation commit changes only `#include` lines and `CMakeLists.txt` paths. Before it:

```powershell
git add -A
.\tools\check_moves.ps1                                        # differences from phase2-baseline are renames and include lines only
.\tools\baseline.ps1 -Against docs\dev\baseline.txt          # exports, CTest by name, fgcheck and regression verdicts
```

If `check_moves` fails, the named file changed beyond its includes: undo that edit, or record
it as a finding for the next phase. If the baseline differs, the commit is wrong: revert it.
Do not fix bugs inside a move commit.

## Commit convention

Conventional Commits. A move commit is `refactor(<module>): ...` with a body listing the moves,
"Changed: #include lines and CMake paths only", the baseline result and "No behaviour change".

## Bugs

Every defect found gets a `K` entry in `docs/refactor/known-issues.md` in the same commit or
before moving on, and the counts in `MILESTONES.md` are updated (see `AGENTS.md`).

## Developer checks

One command, one table of PASS/FAIL rows. Nothing is downloaded; the tools must already be installed
(Visual Studio 2022 with the C++ workload, LLVM in `C:\Program Files\LLVM`). Vendored code in
`third_party/` is never formatted, analysed or linted.

```powershell
.\tools\check.ps1 -Quick       # format, warnings as errors, layering (about 2 minutes, no GPU)
.\tools\check.ps1 -Full        # adds /analyze, clang-tidy, ASan, UBSan
.\tools\check.ps1 -Only tidy   # one step
cmake --build build --target format   # rewrites src/ and tests/ with clang-format
```

Each step builds in its own `build-check-<step>` folder, so `build/` and the shipped DLL are untouched.
Logs are in `build-check-logs/`.

| Step | Tool | Answers the question |
|---|---|---|
| format | clang-format (`.clang-format`) | does the code match the house style? |
| warnings | MSVC `/W4 /WX` (`-DSEDICI_WERROR=ON`) | does the compiler have any complaint? |
| layers | `tools/check_layers.ps1` | does any include point from a low layer up to a higher one? |
| analyze | MSVC `/analyze` (`-DSEDICI_ANALYZE=ON`) | null use, buffer overruns, uninitialised values, ignored results |
| tidy | clang-tidy (`.clang-tidy`, `-DSEDICI_TIDY=ON`, Ninja) | bug-prone patterns, data races, wasteful copies |
| asan | clang-cl AddressSanitizer (`-DSEDICI_SANITIZE=address`) | heap overflow, use after free, double free |
| ubsan | clang-cl UBSan (`-DSEDICI_SANITIZE=undefined`) | undefined behaviour (overflow, bad casts, misaligned access) |
| leaks | `tests/common/leak_check.h` (`-DSEDICI_CRTDBG=ON`) | CPU heap blocks and DXGI/D3D12 objects still alive at exit (the K169-style reference leak) |

How to read a failure: open the log named in the table. A `/analyze` or clang-tidy finding is either a real
defect (fix it and add a `K` entry) or a false positive (suppress that one line with a comment saying why;
never turn a check off for the whole tree). The sanitizer builds use the dynamic runtime (`/MD`) and cover
the test hosts only, never the shipped DLL. `leaks` needs a Debug build of the tests for the CPU part.
