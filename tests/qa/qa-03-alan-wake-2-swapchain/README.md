# QA-03 Alan Wake 2: swapchain create

**Result:** **Partial.** First run (dead on boot, K100): fixed. Second run (2026-11-09): create accepted, game boots, generation context created; the scene is black (K138 fixed afterwards, K139 open; finding 016).

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Alan Wake 2 (Epic Games install), 2560x1080, windowed, back buffer `R10G10B10A2_UNORM`, default settings in `sedici.toml`.

## What was done

- Started the game with a fresh log, twice.

## Observations (owner)

- Current status: dead on boot.
- A worktree named `aw2-patch` (branch `bearingme/aw2-patch`) has changes that make the game boot; see its finding 015. Not merged here.

## Second run (2026-11-09, build of 09:35)

- Owner: "game booted, black screen when rendering the scene (Cyberpunk is working normally)".
- The log (`qa-03-sedici-2.log`) shows the `New` create accepted, 40 s of menu at about 150 real fps, then the generation context created at 48 s and Alan Wake 2's per-frame sequence; presents stop reaching the wrapper from about 46 s. Details and the defect found: [finding 016](../../../docs/findings/016-alan-wake-2-black-screen.md), K138, K139.
- Retest with the fixed build is QA-17.

## Findings

| ID | Finding |
| --- | --- |
| K100 | `ffxCreateContext` with `FrameGenerationSwapChainNewDX12` (0x30005) is refused with `UNKNOWN_DESCTYPE` (result 2); the game never reaches frame generation. |

## Evidence in this folder

| File | What it is |
| --- | --- |
| `qa-03-notes.md` | The owner's notes, tidied. |
| `qa-03-sedici.log` | Log of both starts, each with one refused `ffxCreateContext`. |
| `qa-03-sedici.full.log` | Identical full log (local only). |
| `qa-03-sedici-2.log` | Excerpt of the second run. |
| `qa-03-sedici-2.full.log` | Full log of the second run (local only). |
