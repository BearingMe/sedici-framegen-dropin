# QA-10 VRR presenter and readback

**Result:** **Pass.**

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

LG 29-inch ultrawide (the exact model is not known, there are about three variants), 2560x1080 at 75 Hz, VRR range 40-75. `present = "vrr"`, no overlay hooking.

## What was done

- No separate run. QA-01 to QA-09 were run on VRR, and the 10.6 minute run of QA-05 is the evidence.

## Observations (owner)

- QA-01 to QA-09 were run on VRR; QA-09 was the one expected to fall back to vsync.

## Findings

| ID | Finding |
| --- | --- |
| - | The QA-05 log shows `vrr presenter ready`, 96 periodic `vrr present` lines, `guard timeouts=0` and no tear or late-flip warning. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-10-notes.md` | The owner's notes, tidied. |
| `(see QA-05)` | [../qa-05-multi-second-stalls/qa-05-sedici.log](../qa-05-multi-second-stalls/qa-05-sedici.log) |
