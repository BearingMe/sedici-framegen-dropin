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
