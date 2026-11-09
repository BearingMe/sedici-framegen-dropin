# QA-11 Window changes in game

**Result:** **Pass, with findings.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, anti-lag on, RTSS on (so the presenter was vsync), 2x preset.

## What was done

- Started a new deploy at 2560x1080 borderless.
- Changed to fullscreen.
- Changed the resolution to 1080p.
- Changed the resolution to 720p.
- Changed back to 2560x1080.
- Changed to borderless window.
- Alt-tabbed to write the summary.

## Observations (owner)

- Every change was survived; a new frame generation context was created at each size.
- With the 2x preset the output is capped at 74 Hz. This only happens with frame generation; in the menu the fps goes higher.
- The input frames start at over half the output (about 60 fps) and take some time to go down to 38 fps.

## Findings

| ID | Finding |
| --- | --- |
| K98 | Three more `prepare slot is still in flight` errors returned to the game, each shortly after a size change. |
| K122 | Gaps with no prepare of 24.4, 12.1, 15.6 and 6.3 s during mode changes. |
| K123 | The real rate takes several seconds to settle at 37.5 fps. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-11-notes.md` | The owner's notes, tidied. |
| `qa-11-sedici.log` | Excerpt; all contexts #1 to #5, rejections and long stalls are kept. |
| `qa-11-sedici.full.log` | Full log (local only). |
