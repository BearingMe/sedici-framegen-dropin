# QA-16 Menus and UI-only scenes

**Result:** **Pass, with findings** (Cyberpunk 2077 only).

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window.

## What was done

- Opened the menus and loading screens with frame generation on.

## Observations (owner)

- No frame is generated in: the main menu, the character menu, the in-game menus (settings, graphics and so on), and the map. In general, nothing that stops the main gameplay.
- The loading screen does not run frame generation at first; once loading finishes it starts doing so.
- Frame generation on the loading screen is apparently caused by the game rendering scenes under it.
- This causes a strange effect: borders show in movement on the loading screens, maybe caused by motion vectors working without real input.
- Not every game behaves like Cyberpunk in menus; Alan Wake 2 was not tested (blocked by K100).

## Findings

| ID | Finding |
| --- | --- |
| K128 | Generation runs over the scene under the loading screen, with visible borders in moving areas. |
| K122 | Gaps of 42.9, 4.9, 2.6 and 1.9 s during menus and loading, where the game does not call frame generation. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-16-notes.md` | The owner's notes, tidied. |
| `qa-16-sedici.log` | Excerpt of the run log. |
| `qa-16-sedici.full.log` | Full log (local only). |
