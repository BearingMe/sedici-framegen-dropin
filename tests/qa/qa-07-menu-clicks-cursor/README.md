# QA-07 Menu clicks and Full-menu cursor path

**Result:** **Pass.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window. Only the desktop resolution was tried.

## What was done

- Used the Full menu in the foreground.

## Observations (owner)

- Resolution 2560x1080 at 75 Hz, borderless window. Everything is working.
- Request: the 0.75x menu scale is not useful. Offer 0.8, 0.9 and 1.0.

## Findings

| ID | Finding |
| --- | --- |
| FR-04 | Menu scale choices 0.8, 0.9 and 1.0 only. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-07-notes.md` | The owner's notes, tidied. |
| `qa-07-screenshot.png` | Screenshot of the Full menu in use. |
