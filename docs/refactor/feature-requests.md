# Feature requests

Requests from the owner's in-game QA runs (`qa/`) and ideas that came out of them.
They are **not scheduled**: the owner decided (2026-11-08) to make the current system
stable before expanding it. A request becomes work only when the owner moves it into a
milestone. Defects are not listed here; they go to
[known-issues.md](known-issues.md).

| ID | Request | Source | Notes | Status |
| --- | --- | --- | --- | --- |
| FR-01 | The overlay FPS-limit slider should step in a more useful size. Today it snaps to 0.5 fps, which is too fine to raise the cap by dragging. | Owner, QA-01 ("increases only by 0.5") | Wording is ambiguous: it may also mean "raising only works in 0.5 steps" (that is the bug K118). To confirm with the owner. Likely an input box or +/- buttons next to the slider. | Proposed |
| FR-02 | The FPS limit gets an **Apply** button instead of taking effect while the slider moves. | Owner, QA-01 | Fits the existing Save/Cancel transaction. Apply would set the live cap once; Save would still write the file. Fix K118 in the same change. | Proposed |
| FR-03 | A script that skips the Cyberpunk 2077 intro videos. | Owner, QA-04 | Outside the DLL. A game-side tool is not frame generation and the project rule is no engine patches. It would be a separate `tools/` script that touches game files or start options, so it needs an owner decision and its own safety rules (back up, restore, never launch the game). | Needs owner decision |
| FR-04 | Menu scale choices should be 0.8, 0.9 and 1.0 only. Drop 0.75, 0.5 and the values above 1. | Owner, QA-07 ("0.75x scale is already bullshit") | Today the list is 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 (`ui.cpp`, `draw_scale_combo`). Check the owner's high-DPI case first: with `ui.scale = "auto"` larger values may still be needed on 4K. Existing `sedici.toml` values outside the list must still load (snap to the nearest). | Proposed |
| FR-05 | Warn or auto-cap when the real frame rate times the multiplier is above the display refresh. | Agent, from the QA-02/QA-04/QA-05 logs (not requested by the owner) | On the owner's 75 Hz display the output stays near 73-74 fps, which is the display limit, not a fault. With about 100 real fps and 2x, most generated frames are dropped (for example `generated{shown=95 dropped=129}`). The log already prints `OVER_REFRESH: cap the real rate at N fps`. The request is to show that in the menu or apply `limiter.fps = refresh / multiplier` when the owner opts in. | Proposed |

## Not requests, but worth knowing

- "2x was capped to 73~74 fps" (QA-02, QA-05) matches the 75 Hz refresh. The owner
  suspected anti-lag; QA-05 showed the cap with anti-lag off, so it is not anti-lag.
