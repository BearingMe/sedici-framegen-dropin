# QA-04 Display cadence at the target ratio

**Result:** **Partial.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, 2x, game capped to 30 fps.

## What was done

- Locked the game to 30 fps with 60 fps output and looked at a wall.
- Turned frame generation off in the overlay, then relaunched.

## Observations (owner)

- 30 fps locked with 60 fps output looked stable.
- Turning frame generation off stops generating frames immediately, but the overlay still works.
- At the next launch the overlay did not work.
- No present-interval capture and no frame-generation-off baseline were taken, so the cadence criterion is not proven.

## Findings

| ID | Finding |
| --- | --- |
| K119 | Overlay dead at the next launch after turning frame generation off; the log and `sedici.toml` of that launch were not captured. |
| K121 | Prepare stalls of about 55-60 ms at 8 s, 17 s and 28 s. |
| FR-03 | Script to skip the Cyberpunk intro (outside the DLL; needs an owner decision). |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-04-notes.md` | The owner's notes, tidied. |
| `qa-04-sedici.log` | Excerpt of the run log, including the `OVER_REFRESH` hints. |
| `qa-04-sedici.full.log` | Full log (local only). |
