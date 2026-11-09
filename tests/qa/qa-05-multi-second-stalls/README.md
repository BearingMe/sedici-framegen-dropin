# QA-05 Multi-second stalls

**Result:** **Partial.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window. Run length 10.6 minutes (636 s) on the VRR presenter.

## What was done

- Played without a frame cap or anti-lag.
- Played with a 30 fps cap (no anti-lag).
- Played with anti-lag (no cap).
- Went through demanding areas, so a base frame rate under 30 fps there is not a Sedici fault.

## Observations (owner)

- With default settings the generated output was capped at about 73-74 fps, and this is not related to anti-lag.
- The stretches with no generated frames are Cyberpunk's menus, where the game does not apply frame generation (see QA-16).

## Findings

| ID | Finding |
| --- | --- |
| K121 | Short prepare stalls, and one 48.8 s gap at the end of the run (probably a loading screen or pause). |
| K98 | 15 `prepare slot is still in flight` rejections in this run. |
| K124 | Overlay disabled by an exception 1.7 s before the destroy calls at exit. |

This run is also the evidence for [QA-10](../qa-10-vrr-presenter/README.md).

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-05-notes.md` | The owner's notes, tidied. |
| `qa-05-sedici.log` | Excerpt; the lines around `overlay disabled` at 634.9 s are kept. |
| `qa-05-sedici.full.log` | Full log (local only). |
| `Missing` | Frame-time capture and a list of stall times were not recorded. |
