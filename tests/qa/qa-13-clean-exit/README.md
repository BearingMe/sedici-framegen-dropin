# QA-13 Clean exit and teardown

**Result:** **Pass, with findings.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window.

## What was done

- First run: exit from the Mini menu.
- Second run: exit from the Full menu.
- Third run: exit from the Full menu with the game running.
- Fourth run: Alt+F4 in game. With Full open Alt+F4 does nothing; with Mini it works.

## Observations (owner)

- Closing while frames are being generated is not possible through the game, because the game's menu has frame generation off.
- Runs 1 to 3: both `ffxDestroyContext` calls returned OK, with no overlay exception (K124 did not reproduce).
- Run 4: the context destroy was refused with `destroy before GPU retirement` and returned `ERROR`; the swapchain destroy returned OK. The game exited.

## Findings

| ID | Finding |
| --- | --- |
| K126 | Alt+F4 does nothing while the Full menu is open. |
| K127 | Context destroy refused with ERROR at Alt+F4 during gameplay. |
| K124 | Overlay exception at exit seen in QA-05 only. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-13-notes.md` | The owner's notes, tidied. |
| `qa-13-sedici-runs-1-3.log` | Excerpt of the three menu exits. |
| `qa-13-sedici-runs-1-3.full.log` | Full log (local only). |
| `qa-13-sedici-run-4.log` | Excerpt of the Alt+F4 run, including the refused destroy. |
| `qa-13-sedici-run-4.full.log` | Full log (local only). |
