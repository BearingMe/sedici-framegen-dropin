# QA-01 notes

## 1. Cyberpunk restored to its initial state
- Ran `.\tools\restore.ps1 -Game cp2077`.
- `sedici.toml` was removed.
- `sedici.log` was removed.
- The description of `amd_fidelityfx_framegeneration_dx12.dll` confirms it was built by AMD.

## 2. First deploy
- Ran `.\tools\deploy.ps1 -Game cp2077`.
- `amd_fidelityfx_framegeneration_dx12.dll` no longer carries the AMD description.
- `amd_fidelityfx_framegeneration_dx12.dll.orig` was created.
- `sedici.toml` was created.
- Changed `limiter.fps` to 48 and the multiplier to 1.5x.

## 3. First run
- The overlay works properly.
- Output confirmed at 72 fps through the Steam metrics.
- Visual fluidity confirmed.
- `sedici.log` was created.

Bug:
- The overlay frame limit works when reducing the frame rate, but not when increasing it.

Feature requests:
- The overlay frame limit only moves in 0.5 steps.
- The overlay frame limit should have an Apply button instead of applying right away.

## 4. Second deploy
- `sedici.log` and `sedici.toml` were kept untouched.
- `amd_fidelityfx_framegeneration_dx12.dll.orig` had no changes.
