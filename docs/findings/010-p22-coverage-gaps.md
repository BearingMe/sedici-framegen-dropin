# 010: P2.2 coverage and accepted gaps

> **Status (2026-11-09): historical partition.** The gap split is still the rationale for
> which milestone owns what, but the test count has grown (37 registered CTest entries)
> and P2.6 and P2.8 have since been accepted, so "Closed in" names an owner, not a
> status.

Records what the P2.2 regression and integration coverage proves, what it
deliberately does not, and where each gap is closed. The refactor-side table is
[reliability.md](../refactor/reliability.md#p22-accepted-gaps); this file is the
evidence record. Nothing here has run in the game.

## What is covered

18 CTest entries pass on one shared `sedici_core` object library (the DLL and
every test host build the same production objects):

- 12 `sedici_abi` scenarios against the built DLL and a stub `.orig`: exports,
  forward, pacing, async-off, missing-orig, bad-config, session, window-replace,
  destroy-inflight, log-failure, invalid-calls, overlay-hotkey. The lifecycle
  scenarios keep up to three frames of GPU work in flight and count debug-layer
  errors.
- 3 `sedici_fgcheck`, `sedici_regression`, and two GPU overlay checks.

## Accepted gaps

Each needs a seam production code does not have yet. Building it as a test-only
hack now would test the hack, so each is assigned to the milestone that adds the
seam.

| Gap | Why it is not covered | Closed in |
| --- | --- | --- |
| Full-menu clicks: cap change, Save, Cancel, settings round trip | Immediate-mode ImGui, no stable coordinates and no UI model. Full input also needs a foreground host window, which an unattended CTest run lacks (`overlay-hotkey` logs an INFO line). Hotkey, Full draw, clip restore and the Save path are exercised; only the click is missing. | P2.6, P2.8 |
| GPU allocation and submission failures (device removed, `CreateCommittedResource` / `ExecuteCommandLists` failing) | No portable fault injection on a real D3D12 device; needs a device/queue abstraction. | P2.5 |
| Save-time failures (read-only or externally edited `sedici.toml`) | Reachable only through the Save button. `config_save` itself is covered in `sedici_regression`. | P2.6 |
| Chronological readback through the vrr presenter | The presenter hands images to DXGI, not a readable queue; readback exists only for the old path. | P2.7 |
| Production-resolution performance | Needs a measured run on the target GPU, not a pass/fail test. | P2.11 |

Also not exercised: creating a second wrapped swapchain while the first is alive.
The game destroys the first session before creating the next.

## Known failures (`sedici_fgcheck --present`, expected-fail)

- `generated presentation`: `"vsync"` path, 3 of 5 midpoints skipped as late.
- `session`: `"vsync"` path through `fg::Session`, frame 2 midpoint reads back at
  16.02 dB.

Both fail identically at `a87d145` and were hidden until RTSS was closed
([009](009-output-multipliers.md)).
