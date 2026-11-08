# QA: in-game acceptance tests (P2.11)

These are the checks that need the game, a display or an external overlay. An agent
never launches the game, so you run them. Deployment and launch need your explicit
authorization each time.

Source of truth for the milestone: [MILESTONES.md](../MILESTONES.md), section P2.11.
Known issues referenced below are in [known-issues.md](../docs/refactor/known-issues.md).

## How to run

1. Close the game. Deploy the current build with `tools/deploy.ps1` (QA-01).
2. **Delete `sedici.log` before each test** so the log only holds the build under
   test (K101). Copy it into the test's folder right after the run.
3. Set `[log] level = 1` in `sedici.toml` unless the test says otherwise.
4. Put evidence in the folder named after the test ID, for example `qa/QA-04/`.
   Name files `<ID>_<what>.<ext>`, for example `QA-04_present-intervals.csv`.
5. Fill in the Result column (pass, fail, blocked) and a one-line note.

Evidence files are ignored by git (see `qa/.gitignore`) because logs and captures can
contain usernames and paths. Only this README and the `.gitkeep` files are tracked.
Scrub a log before pasting it anywhere public.

## Tests

| ID | Title | Description | Evidence needed | Deferred from | Result |
| --- | --- | --- | --- | --- | --- |
| QA-01 | Deploy | With the game stopped, run `tools/deploy.ps1` for the target game. Confirm the original DLL was backed up as `.orig`, Sedici's DLL is in place, and an existing `sedici.toml` was not overwritten. Run deploy a second time and confirm it refuses to overwrite `.orig`. | Console output of both deploy runs; a directory listing of the game folder after deploy (DLL, `.orig`, `sedici.toml`). | P2.9 | **Pass, with findings.** Restore, first deploy, first run (72 fps, overlay works) and second deploy (log, toml and `.orig` untouched) all as expected. Bug K118; requests FR-01, FR-02. The original DLL hash was not recorded: note it before the next deploy for QA-15. |
| QA-02 | Cyberpunk 2077: generation runs | Start the game with a fresh `sedici.log`. Load into a scene with motion, play 5 minutes with frame generation on, then quit normally. Pass if there is no crash, generated frames are visible, and the log has no errors, rejections ("rejected") or setup failures ("could not"). | `sedici.log` for the run; one short screen capture showing frame generation on; game build version. | all, K101 | **Pass, with findings.** Generation visible, no crash. Output sits at 73-74 fps because the display is 75 Hz (FR-05), not anti-lag. Disocclusion at high speed (K120); short prepare stalls in the log (K121). Run length not recorded. |
| QA-03 | Alan Wake 2: swapchain create | Start Alan Wake 2 with a fresh log. Pass if the `SwapChainNewDX12` create is accepted, frames present, and the log shows no refusal. If it is refused, keep the log: the exact message and return code are the finding. | `sedici.log`; a short capture of gameplay; game build version. | K100, [finding 014](../docs/findings/014-alan-wake-2-swapchain-new.md) | **Fail (K100).** Dead on boot: the `SwapChainNewDX12` create is refused with `UNKNOWN_DESCTYPE`. The `aw2-patch` worktree has changes that boot the game; not merged. |
| QA-04 | Display cadence at the target ratio | In a steady scene (no loading, fixed camera movement), cap the game to 30 real fps and run 2x for 2 minutes. Pass if output stays steady at about 60 fps with no collapse to 20 fps or to 2x of a lower rate. Then repeat once with frame generation off at the same game settings as the baseline. | A present-interval capture (PresentMon, CapFrameX or similar) for the FG run and for the baseline, as CSV; the `sedici.log` for the FG run, including the periodic stats and `diag:` lines. | P2.7, K95 | **Partial.** By eye 30 fps locked to 60 output looked stable and FG off stops generating at once. No present-interval CSV and no FG-off baseline, so the cadence criterion is not proven. Bug K119 (overlay dead on next launch); request FR-03. |
| QA-05 | Multi-second stalls | Play 15 minutes with traversal, fast turns, a loading or streaming zone, and menu opening and closing. Note the time of every stall of more than 1 second. Pass if there are no stalls of several seconds, or each stall is explained in the log (slot rejection, K98, or K99). | `sedici.log`; a frame-time capture covering the whole run; a short text list of stall times. | P2.7, K98, K99 | **Partial.** Three setups tried (no cap, 30 fps cap, anti-lag). No stalls reported by the owner, but no frame-time capture or stall list, and the log has one 48.8 s prepare gap and short stalls (K121). Part of the 30 fps run shows `generated{shown=0}` for about 60 s: owner to say whether FG was off. |
| QA-06 | Hotkey reliability | Press Alt+F10 at least 30 times (Off, Mini, Full, Off cycle), some quickly and some during heavy load. Pass if every press changes the mode once, there is no timeout, and no key stays stuck down after release. | `sedici.log`; a note of presses made and presses missed; a screen capture of one full cycle. | P2.8, K54 | **Pass, provisional.** Owner saw no missed press. Number of presses not recorded; the level-1 log does not show hotkey events. |
| QA-07 | Menu clicks and Full-menu cursor path | Open the Full menu with the game window in the foreground. Click each control, drag a slider, and move the cursor to every screen edge. Pass if clicks land where the cursor is (also at a resolution other than the desktop's), the game does not receive the clicks, and after closing the menu the cursor and its clip are exactly as before. | A screen capture of the Full menu in use; the `sedici.log`; a note of the resolution and window mode used. | P2.2, P2.8, K36 | **Pass.** 2560x1080 at 75 Hz, borderless window, everything works. Only the desktop resolution was tried. Request FR-04. |
| QA-08 | Settings: cap change, save, cancel | In the Full menu change the limiter cap, press Cancel and confirm the old value returns; change it again, press Save and confirm the cap takes effect in play and `sedici.toml` holds the new value with its comments intact. Restart the game and confirm the value persists. | `sedici.toml` before and after Save; `sedici.log`; a short capture showing the cap taking effect. | P2.6, K36 | |
| QA-09 | External overlay compatibility | Run the game with RTSS (RivaTuner) on and its on-screen display enabled, then with RTSS off. Pass if there is no crash or device removal, the log states the presenter choice under RTSS (the "vsync" fallback, K13), and generation still works. Optional: repeat with `SEDICI_FG_ALLOW_RTSS=1` only if you accept the removal risk. | `sedici.log` for each of the two runs; a capture of the RTSS run; RTSS version. | P2.7, P2.8, K13 | |
| QA-10 | VRR presenter and readback | On a VRR display, with `present = "vrr"` and no overlay hooking, run 5 minutes. Pass if the log shows `vrr presenter ready` and periodic `vrr present` statistics, with no late-flip or tear warnings that persist. Record the refresh rate and whether VRR is active in the driver. | `sedici.log`; a present-interval capture; display model, refresh rate and driver VRR setting. | P2.7, P2.8 | |
| QA-11 | Window changes in game | While generation is on: toggle fullscreen and windowed, change the resolution, alt-tab away and back, and change the display mode if the game allows. Pass if each change is survived with no crash, frames keep presenting, the overlay and cursor recover, and FG resumes. | `sedici.log`; a note of each change made and what happened; a short capture of at least one change. | integrated scenario | |
| QA-12 | Observer overhead | Run the same 3-minute scene twice with the same settings: once with `[log] level = 0` and the overlay Off, once with `[log] level = 2` and the Full menu open. Pass if the average and 1% low frame times differ by no more than normal run-to-run noise; if not, record the gap. | Two frame-time captures (CSV) and the `sedici.toml` used for each; the `diag:` lines from the second log. | P2.10 | |
| QA-13 | Clean exit and teardown | Quit the game normally from Mini and from Full menu (two runs), and once by closing the window during generation. Pass if the process ends within a few seconds with no hang, no crash dialog, the cursor clip is released, and the last log lines show the context and swapchain being destroyed. | `sedici.log` from each of the three exits; a note of how long each exit took. | P2.10 | |
| QA-14 | Hung settings volume (optional) | Only if you have a safe way to stall a drive (for example a disconnected network share holding the game folder). Open the Full menu and press Save while the volume hangs. Pass if the menu reports "Settings file is not responding" within about 5 seconds, the game keeps running, and the menu works again once the volume returns. Skip if you cannot do this without risk to your data. | `sedici.log`; a capture of the status message; a note of how the stall was created. | P2.10, K83 | |
| QA-15 | Rollback | With the game stopped, run `tools/restore.ps1`. Confirm the original DLL is back, the `.orig` file was not modified (compare its hash with the one noted at QA-01), and `sedici.toml` and `sedici.log` handling matches the deploy rules. Start the game once to confirm it runs with the original DLL. | Console output of the restore; hashes of the original DLL before deploy and after restore; a directory listing of the game folder. | P2.9 | |

## Order

Run QA-01 first and QA-15 last. QA-02 to QA-13 can run in any order on one deployment;
group the ones that need the same game together. QA-14 is optional.

## Reporting a failure

Do not edit the build to make a test pass. Keep the evidence, mark the Result as
fail, and tell the agent the test ID: every defect becomes a new entry in
`docs/refactor/known-issues.md` with the evidence marked as measured.
