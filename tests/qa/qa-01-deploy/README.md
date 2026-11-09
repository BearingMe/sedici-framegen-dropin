# QA-01 Deploy

**Result:** **Pass, with findings.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window. Commands: `tools/restore.ps1 -Game cp2077`, then `tools/deploy.ps1 -Game cp2077`.

## What was done

- Restored Cyberpunk to its initial state with `tools/restore.ps1 -Game cp2077`: `sedici.toml` and `sedici.log` were removed, and the DLL description shows it is built by AMD.
- First deploy with `tools/deploy.ps1 -Game cp2077`: the DLL no longer has the AMD description, `amd_fidelityfx_framegeneration_dx12.dll.orig` was created, `sedici.toml` was created.
- Edited the new `sedici.toml`: `limiter.fps` to 48 and the multiplier to 1.5x (the log shows ratio 3/2).
- First run, then second deploy.

## Observations (owner)

- The overlay works properly.
- Output confirmed at 72 fps through the Steam counter, and the game looks fluid.
- `sedici.log` was created.
- Second deploy: `sedici.log` and `sedici.toml` untouched, and `.orig` unchanged.
- Bug: the overlay frame limit can be lowered but not raised.
- Request: the overlay frame limit only moves in 0.5 steps and is applied at once; wanted an Apply button.

## Findings

| ID | Finding |
| --- | --- |
| K118 | FPS-limit slider lowers the cap but does not reliably raise it. |
| FR-01 | Clamped 0.5 fps steps on the slider. |
| FR-02 | Apply button for the FPS limit. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-01-notes.md` | The owner's notes, tidied. |
| `qa-01-sedici.log` | Excerpt of the first-run log: limiter 48 fps, schedule 1.5x, vrr presenter, overlay hooks. |
| `qa-01-sedici.full.log` | Full log (local only, not in git). |
