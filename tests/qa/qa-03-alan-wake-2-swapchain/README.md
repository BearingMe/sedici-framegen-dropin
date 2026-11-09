# QA-03 Alan Wake 2: swapchain create

**Result:** **Fail** (K100).

Back to the [test table](../README.md). Bugs are in [known-issues.md](../../../docs/refactor/known-issues.md); requests in [feature-requests.md](../../../docs/refactor/feature-requests.md).

## Setup

Alan Wake 2 (Epic Games install), 2560x1080, windowed, back buffer `R10G10B10A2_UNORM`, default settings in `sedici.toml`.

## What was done

- Started the game with a fresh log, twice.

## Observations (owner)

- Current status: dead on boot.
- A worktree named `aw2-patch` (branch `bearingme/aw2-patch`) has changes that make the game boot; see its finding 015. Not merged here.

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
