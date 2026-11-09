# QA-12 Observer overhead

**Result:** **Pass on the offline measurement** (the in-game benchmark was too noisy to show it).

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

## Offline measurement (closes this test)

The in-game benchmark cannot show the cost, so it was measured offline:
`build\tests\Release\sedici_regression.exe observer` (2560x1080 output, no debug layer, RX 7700 XT, 300 frames per row, three rounds, 2026-11-09). It times `ui_on_present` and the whole record-submit-wait round trip per presented frame.

| Overlay | CPU in `ui_on_present` (avg, p99) | Round trip (avg, p99) |
| --- | --- | --- |
| Off | 0.2 us, 0.2 us | 5.7-5.8 us, 8-10 us |
| Mini | 19-21 us, 35-39 us | 121-133 us, 403-459 us |
| Full | 29-35 us, 52-73 us | 126-157 us, 242-477 us |

Logging: `INFO log overhead: 0.3 us average, 3.7 us p99 per accepted line` (`sedici_regression`). The QA-12 run wrote 334 lines in 824 s (0.4 lines per second), so logging costs the game thread well under a millisecond per hour.

At 75 fps a frame is 13,333 us: Full is under 0.3 percent of CPU time on the calling thread and about 1 percent of the frame as a round trip. This is far below the 15 percent noise of the in-game benchmark, which is why that benchmark cannot see it. Limit: the offline run draws the menu without a real game on the GPU, so the GPU share in a heavy scene is not measured; the in-game rerun with a steady scene stays optional.
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
