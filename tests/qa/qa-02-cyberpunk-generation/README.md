# QA-02 Cyberpunk 2077: generation runs

**Result:** **Pass, with findings.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, default settings from a fresh deploy (2x, limiter off, `ui.mode = "off"`).

## What was done

- Fresh deploy with the default `sedici.toml`.
- Played with frame generation on and quit normally. Run length was not recorded.

## Observations (owner)

- 2x output was capped at about 73-74 fps. The owner first suspected anti-lag; QA-05 showed the same cap with anti-lag off. It matches the 75 Hz refresh.
- Disocclusion artefacts appear with high-speed movement and are not visible at normal speed.

## Findings

| ID | Finding |
| --- | --- |
| K120 | Disocclusion artefacts at high speed (Stage 1 limit; no capture saved). |
| K121 | Short prepare stalls of 45-65 ms every few seconds in the log. |
| FR-05 | Warn or auto-cap when real fps times the multiplier is above the refresh. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-02-notes.md` | The owner's notes, tidied. |
| `qa-02-screenshot.png` | Game screenshot with frame generation on. |
| `qa-02-sedici-settings.toml` | The `sedici.toml` used for the run. |
| `qa-02-sedici.log` | Excerpt of the run log. |
| `qa-02-sedici.full.log` | Full log (local only). |
