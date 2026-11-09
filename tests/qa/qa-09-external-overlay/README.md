# QA-09 External overlay compatibility

**Result:** **Pass.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, RTSS (RivaTuner) with its on-screen display on.

## What was done

- QA-01 to QA-09 were run without RTSS, so those runs are the baseline. No crash in any of them.
- A separate run with RTSS on provided the log in this folder.

## Observations (owner)

- With RTSS the log says the `vrr` presenter is unavailable (RTSS is injected) and falls back to `vsync`, as designed (K13).
- No crash, and generation still works (about 150 generated frames per 300).
- Under vsync, prepare stalls of 84-113 ms at a median of 26.7 ms recur every few seconds.
- `SEDICI_FG_ALLOW_RTSS=1` was not tried.

## Findings

| ID | Finding |
| --- | --- |
| K121 | Prepare stalls under the vsync fallback. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-09-notes.md` | The owner's notes, tidied. |
| `qa-09-sedici-rtss-on.log` | Excerpt of the RTSS run log. |
| `qa-09-sedici-rtss-on.full.log` | Full log (local only). |
