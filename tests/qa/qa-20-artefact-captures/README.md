# QA-20 Artefact captures: disocclusion, loading screen, stalls, overlay at next launch

**Result:** **Not run.** Needs the game; prepared in P4.7.

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md). Verdict rules: [phase4-game-checklist.md](../../../docs/refactor/phase4-game-checklist.md).

Four defects need evidence the owner's earlier runs did not keep: K120 (disocclusion at speed),
K128 (borders on the loading screen), K121 (short stalls) and K119 (overlay dead at the next launch).
This test is only about keeping the evidence.

## Before every run

1. Close the game. `tools\deploy.ps1` for the game (QA-01 rules), note the DLL hash it prints.
2. **Delete `sedici.log`** beside the game (K101). The first lines of the new log carry the build date.
3. `sedici.toml`: `[log] level = 1` (level 2 only where a step says so). Keep a copy of the file you used.
4. Note the display model, refresh rate, VRR on or off in the driver, and the game's resolution and window mode.

Record the session with any screen recorder (or take screenshots), and start a stopwatch when the game
reaches gameplay so every note has a time. Log timestamps are milliseconds since the first FFX call, so write
down the stopwatch time at which the game made it (the first line of the new log appears then).

## Steps

1. **K120.** Cyberpunk, 2x, a vehicle at high speed along a street with poles or fences. Record 20
   seconds. When the artefact shows, pause the recorder and save two consecutive frames if the recorder
   can step; otherwise a screenshot taken as it happens. Note the stopwatch time.
2. **K128.** Load a save. Record from the loading screen until the game is playable. Note the time
   when generation starts and when the borders show.
3. **K119.** Set the Full menu to Off with the overlay (Alt+F10 to Off), exit, **copy `sedici.toml` and
   `sedici.log`**, start the game again and try Alt+F10. If the overlay is dead copy the new full log and
   `sedici.toml` again before doing anything else. The `overlay` and `interception` lines say which part failed.
4. **K121.** Five minutes of ordinary play (2x, VRR). Run the analyzer on the log: stalls with no
   `slow call` line near them are the game's, not Sedici's.

## Keep in this folder

The recordings or screenshots (game only), the stopwatch notes, the logs as excerpts (full local),
`sedici.toml` before and after for step 3, and the analyzer output.

## Results

| Step | Entry | Result | Notes |
| --- | --- | --- | --- |
| 1 | K120 | not run | |
| 2 | K128 | not run | |
| 3 | K119 | not run | |
| 4 | K121 | not run | |
