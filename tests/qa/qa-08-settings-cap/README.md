# QA-08 Settings: cap change, save, cancel

**Result:** **Pass.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, Full menu.

## What was done

- Changed the limiter cap in the Full menu, cancelled, then changed it again and saved.

## Observations (owner)

- The frame cap is applied right away while it is being changed; Cancel restores the initial value.
- Save wrote `limiter.fps = 24.5`. The before and after files differ in that single line, so the comments are intact.
- Whether the value persisted after a restart was not stated.

## Findings

| ID | Finding |
| --- | --- |
| FR-02 | The cap takes effect before Save; an Apply button was requested. |
| K118 | Slider behaviour. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-08-notes.md` | The owner's notes, tidied. |
| `qa-08-sedici-settings-before.toml` | `sedici.toml` before Save (`fps = 0`). |
| `qa-08-sedici-settings-after.toml` | `sedici.toml` after Save (`fps = 24.5`). |
