# Feature requests

Requests from the owner's in-game QA runs (`tests/qa/`) and ideas that came out of them.
They are **not scheduled**: the owner decided (2026-11-08) to make the current system
stable before expanding it. A request becomes work only when the owner moves it into a
milestone. Defects are not listed here; they go to
[known-issues.md](known-issues.md).

| ID | Request | Source | Notes | Status |
| --- | --- | --- | --- | --- |
| FR-01 | The FPS-limit slider must move in clamped 0.5 fps steps (40, 40.5, 41, and so on). Today the handle moves continuously and the value only snaps afterwards, so landing on a specific value by dragging is very hard. | Owner, QA-01 and follow-up 2026-11-08 | Owner clarified the meaning. Same control as K118: build the slider on whole steps (an integer slider over half-fps units) so the handle jumps from step to step, and consider arrow-key and +/- nudging. | Proposed |
| FR-02 | The FPS limit gets an **Apply** button instead of taking effect while the slider moves. | Owner, QA-01 | Fits the existing Save/Cancel transaction. Apply would set the live cap once; Save would still write the file. Fix K118 in the same change. | Proposed |
| FR-03 | A script that skips the Cyberpunk 2077 intro videos. | Owner, QA-04 | Outside the DLL. A game-side tool is not frame generation and the project rule is no engine patches. It would be a separate `tools/` script that touches game files or start options, so it needs an owner decision and its own safety rules (back up, restore, never launch the game). | Deferred (owner, 2026-11-09): outside Phase 4, unrelated to the system. Research: the intro videos are inside the game's `.archive` files (no loose video files), so a script cannot rename them; the community answer is the "No Intro Videos" archive mod (Nexus mod 533, `basegame_no_intro_videos.archive`). `tools/intro_skip.ps1` installs and removes a copy you downloaded; it never downloads or launches. |
| FR-04 | Menu scale choices should be 0.8, 0.9 and 1.0 only. Drop 0.75, 0.5 and the values above 1. | Owner, QA-07 ("0.75x scale is already bullshit") | Today the list is 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 (`ui.cpp`, `draw_scale_combo`). Check the owner's high-DPI case first: with `ui.scale = "auto"` larger values may still be needed on 4K. Existing `sedici.toml` values outside the list must still load (snap to the nearest). | Proposed |
| FR-05 | Warn or auto-cap when the real frame rate times the multiplier is above the display refresh. | Agent, from the QA-02/QA-04/QA-05 logs (not requested by the owner) | On the owner's 75 Hz display the output stays near 73-74 fps, which is the display limit, not a fault. With about 100 real fps and 2x, most generated frames are dropped (for example `generated{shown=95 dropped=129}`). The log already prints `OVER_REFRESH: cap the real rate at N fps`. The request is to show that in the menu or apply `limiter.fps = refresh / multiplier` when the owner opts in. | Proposed |

## Not requests, but worth knowing

- **Menus differ per game.** Owner, QA-05: Cyberpunk 2077 does not apply frame generation in its menus, so output falls to the real rate while a menu is open. That explains the generated{shown=0} stretches in the QA-05 log. Not every game behaves like this, so QA-16 records it per game. It is expected behaviour, not a defect.
- "2x was capped to 73~74 fps" (QA-02, QA-05) matches the 75 Hz refresh. The owner
  suspected anti-lag; QA-05 showed the cap with anti-lag off, so it is not anti-lag.
