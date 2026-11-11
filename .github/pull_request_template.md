## What and why

<!-- One or two sentences. Link the issue: Closes #N -->

## Type

<!-- The PR title becomes the squash commit, so it must be a Conventional Commit:
feat | fix | refactor | perf | test | docs | ci | build | chore, e.g. `fix: reject a stale handle` -->

## Checklist

- [ ] `tools/check.ps1 -Quick` passes (CI repeats it)
- [ ] `ctest -C Release --output-on-failure` result: <!-- n of n; say which gpu-labelled tests you ran locally -->
- [ ] Still exactly five exports; the descriptor types, return codes and swapchain behaviour are unchanged, or the change is explained above
- [ ] Every bug found along the way is a GitHub issue (Bug template); a bug fixed here is closed with this commit
- [ ] `MILESTONES.md` table and "Last updated" refreshed if a milestone status or the open-bug count changed
- [ ] `third_party/` untouched; no logs, usernames or home paths added
- [ ] The game was not launched

## In-game evidence

<!-- "None needed", or the QA-nn the owner ran. Issues that need a game run carry the `needs-game` label. -->
