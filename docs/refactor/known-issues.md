# Known issues deferred to after the refactor

Status: **recorded, not fixed.** Problems found while building the vrr presenter,
the output multipliers and the P2.2 coverage on `experiment/cobalt-otter`
(through `f95b54d`). The decision is to fix them on the refactored architecture,
not to patch the current code further. Each entry names its evidence and the
milestone expected to own it. "Measured" means a game capture or offline check;
"code" means read from the source; "suspected" means not yet demonstrated.

Entries marked **[arch]** constrain the target design; the resulting notes are in
[architecture.md](architecture.md#constraints-from-known-defects). The static
current-state review is in [audit.md](audit.md); the coverage map and tracked
expected failures are in [reliability.md](reliability.md#current-coverage-and-gaps-p22-baseline).

## Output pacing and latency

| ID | Problem | Evidence | Owner |
| --- | --- | --- | --- |
| K1 [arch] | Adaptive at ~32 real fps: ~55-60 ms latency, almost every flip late, 25-43 generated frames dropped per 300, output ~65 instead of 72 fps, many frames 14.5-20 ms. About half the latency is inherent (an output can sit up to one real frame back); the rest is the GPU-bound game finishing frames ~30 ms after Present. User felt little difference from before the last fix; Cyberpunk's own traversal stutter confounds subjective tests. | Measured: `2026-11-06-sedici-vrr-adaptive-fix*`, [finding 009](../findings/009-output-multipliers.md) | P2.7 |
| K2 [arch] | Generation load feeds back into the real rate: at low real fps adaptive needs two generated frames per real frame, adding GPU work that lowers the real rate further. Nothing budgets generation cost. | Measured (real 30-33 vs 34-44 before; scene may differ), finding 009 | P2.7, P2.11 |
| K3 [arch] | Planned output above the panel's refresh (game uncapped, or real fps x multiplier > refresh) is only reported (`OVER_REFRESH`); the presenter then drops outputs. No component owns "never plan more than the panel can show". | Measured: first 1.5x session; code `present_async` | P2.7 |
| K4 [arch] | With FG off (menus) and the game uncapped, every real frame still goes through the presenter and its tear guard; the game is throttled towards the panel rate (backpressure ~100/300). | Measured: adaptive session logs at 150-220 real fps | P2.7 |
| K5 [arch] | VRR with the tear guard off: Present calls evenly spaced (13.7/13.7 ms) but flips land 10.3/16.9 ms apart, so every second flip happens later than its call. Unexplained; the scan-line guard hides it. | Measured: `2026-11-06-sedici-vrr-guardoff-101324`, finding 008/009 | P2.7 investigation |
| K6 | Pacing heuristics are tuned constants with game evidence but no derivation: hitch at 2.5x the recent interval, latency windows of 8/16 frames, GPU median + 1 ms, real-frames-only below 20 fps, adaptive snap at 1/10 slot, scan-line lead 0.25 ms, early window 1.5 ms. | Code `fg_pacing.cpp`, `fg_presenter.cpp`; finding 009 | P2.7 (pure, fake-time policy tests) |
| K7 | The presenter may drop a real frame when a later output is already due; at 2x the offline check saw 78 of 79 planned presents once. | Measured: CTest run, `sedici_fgcheck_present` | P2.7 |
| K8 [arch] | The game-facing frame-latency waitable object is the native chain's. With presentation on Sedici's thread, its signalling follows Sedici's native presents (1-3 per game frame), not game frames; the game's `SetMaximumFrameLatency(1)` contract is no longer what it requested. Not characterized. | Code `Swapchain::GetFrameLatencyWaitableObject` forwards; suspected | P2.7, P2.8 |
| K9 | `"vsync"` path known failures: `generated presentation` skips 3 of 5 midpoints as late; `session` frame 2 midpoint reads back at 16.02 dB. Both fail at `a87d145`; hidden earlier by RTSS. | Measured: `sedici_fgcheck --present` XFAIL | P2.7 / P2.5 |
| K10 | Diagnostics semantics are loose: `late` counts every guard hold over 1 ms, `short_gaps` flags 12.97 ms gaps on a 13.33 ms panel, `idle_in_blank` rarely samples at ~73 fps and was misread once as "VRR inactive". | Code `Presenter::log_window`; finding 008 correction | P2.10 |

## Swapchain ownership and presentation queue

| ID | Problem | Evidence | Owner |
| --- | --- | --- | --- |
| K11 [arch] | `"vrr"` destroys the game's native chain and recreates it on Sedici's present queue, relying on the wrap desc handing over the game's reference (AMD's behaviour is assumed, not verified against the binary). Recreation copies description, fullscreen state, latency and background colour, but **not** colour space or HDR metadata. If creation fails after the old chain is destroyed and the game-queue retry also fails, the game is left without a chain. | Code `Presenter::adopt` | P2.3, P2.4 |
| K12 [arch] | In `"vsync"`/`"game"` the game's reference to its native chain is never released, which could block the game creating a new chain on the same window after destroying the wrapper. | Code `route_create` (reference handed over only for vrr); suspected, no failing run | P2.4 |
| K13 [arch] | RTSS draws its OSD into back buffers from whichever queue it last saw, not the chain's queue. With a separate present queue that removed the device (2 of 3 offline runs), so `"vrr"` refuses to start while `RTSSHooks64.dll` is loaded and falls back to `"vsync"` (`SEDICI_FG_ALLOW_RTSS=1` overrides). RTSS also reuses a destroyed queue across swapchains, which is why Sedici leaks one present queue per device for the process lifetime. | Measured offline; finding 008 | P2.8, P2.9 |
| K14 [arch] | Wrapping requires an `R8G8B8A8_UNORM` back buffer and a single queue/node; anything else returns `DXGI_ERROR_UNSUPPORTED` and the wrap fails instead of falling back, so an HDR/10-bit game would get a failed FFX call. | Code `initialize_buffers`, `resize` | P2.4 (fallback before ownership) |
| K15 | `"vrr"` adds a full-resolution copy of every real frame (snapshot ring) and holds three slots of generated images; neither cost is measured at production resolution. | Code `snapshot_real`; no perf run | P2.5, P2.11 |

## Overlay, status and settings

| ID | Problem | Evidence | Owner |
| --- | --- | --- | --- |
| K16 | On the Sedici path the overlay is drawn on every copied output, including outputs the presenter then drops, so ImGui frame counts include frames never shown. | Code `copy_to_native` hook | P2.8 |
| K17 | The overlay's first "present callback active" log line on the Sedici path reports `device=0x0 ... size=0x0` because the presented hook carries no resource. | Measured: `sedici_abi session` log | P2.10 |
| K18 | FG on/off status on the Sedici path is set only from routed configures; destroying the Sedici FG context does not reset it (routed destroy returns before `toggles_on_destroy`). | Code `dllmain.cpp` | P2.4, P2.6 |
| K19 | `target_fps = 0` resolves to refresh - 3 once, at wrap time; moving the window to another monitor or changing mode does not update it. | Code `configure_schedule` | P2.6, P2.7 |
| K20 | Hidden environment-only switches remain: `SEDICI_FG_ALLOW_RTSS`, `SEDICI_FG_SYNC`, `SEDICI_FG_ALIGN` (CPU vblank arming, documented as not working), `SEDICI_FG_SCANLINE_LEAD_US`. | Code; [audit](audit.md) | P2.6 |
| K21 | The shipped `sedici.toml` (and `config.h`) say `async.mode = "off"` clears the async flag on create; the code stopped in `6c2c180`. README and `sedici_abi async-off` describe the current behaviour. | Code, ABI test | P2.1 reconciliation |
| K22 | Multipliers other than 2x are silently 2x in `"vsync"`/`"game"` (one log line). | Code `configure_schedule` | P2.6 |

## Test infrastructure

| ID | Problem | Evidence | Owner |
| --- | --- | --- | --- |
| K23 | Offline windows are composed/windowed, never independent-flip fullscreen with VRR, so presentation checks do not represent the game's path; loop timing is dominated by 15.6 ms `Sleep` granularity (an earlier "held to panel pace" claim was this artefact). | Offline checks; finding 009 correction | P2.2 |
| K24 | RTSS injected into test processes skips the vrr checks and has corrupted readbacks; runs must close it. | Offline checks | P2.2 |
| K25 | ~~`sedici_fgcheck` compiles a subset of production sources and `sedici_regression` recompiles them; there is no shared production library target.~~ **Resolved in P2.2:** the DLL, `sedici_regression` and `sedici_fgcheck` all compile one `sedici_core` object library; only `dllmain.cpp`/`exports.def` stay DLL-only. | CMake | P2.2 |
| K26 | Live cap changes, Save/Cancel and Full-menu input are not driven by any test; the combined session checks Mini only. | [reliability.md](reliability.md#current-coverage-and-gaps-p22-baseline) | P2.2 |
