# QA-12 Observer overhead

**Result:** **Inconclusive in game; closed on the offline measurement** (see below).

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Cyberpunk 2077, 2560x1080 at 75 Hz (LG 29-inch ultrawide, VRR range 40-75), borderless window, RTSS off, Cyberpunk's own benchmark. The initial log level 0 was set in `sedici.toml`; every other change was made through the overlay.

## What was done

- Benchmark 1 (log errors, overlay off): 105.71 fps mean, 67.69 min.
- Benchmark 2 (log normal, overlay on): 124.19 fps mean, 100.07 min.
- Benchmark 3 (log errors, overlay off): 122.98 fps mean, 105.20 min.
- Benchmark 4 (log normal, overlay on): 110.72 fps mean, 70.89 min.

## Observations (owner)

- The Cyberpunk benchmark is unreliable: identical settings differ by more than 15 percent.
- The game's fps counter does not match the overlay counter for input.
- The Steam fps counter matches the overlay counter for output.
- The input fps did not settle at 38 during the benchmark (game about 100, overlay about 60).

## Findings

| ID | Finding |
| --- | --- |
| K125 | The overlay's Real counter counts shown real frames; the presenter drops real frames above the refresh. |
| K123 | The real rate does not settle at 38 fps. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-12-notes.md` | The owner's notes, tidied. |
| `qa-12-sedici.log` | Excerpt of the run log. |
| `qa-12-sedici.full.log` | Full log (local only). |
