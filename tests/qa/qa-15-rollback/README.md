# QA-15 Rollback

**Result:** **Pass, provisional.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window. `tools/restore.ps1` and `tools/deploy.ps1`.

## What was done

- Ran restore and deploy around every QA task.

## Observations (owner)

- Working fine: the restored DLL has the same size and description as the original.
- Running restore and deploy for every QA task was very consistent.
- A hash comparison with the original DLL was not recorded.

## Findings

None.

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-15-notes.md` | The owner's notes, tidied. |
| `(none)` | Notes only; no separate evidence files. |
