# Behaviour baseline

`docs/dev/baseline.txt` is a record of the whole offline suite for the Rust build, produced by
`tools/baseline.ps1`. Compare against it before and after a slice: a difference is either a change
the slice is meant to make, or a regression.

The C++ equivalent (the CTest record of tag `phase2-baseline`) retired with the C++ build; the
Phase 3 move-checks it supported (`tools/check_moves.ps1`, `tools/check_layers.ps1`) are gone too
(the module layering is now `tests/architecture.rs`). `docs/dev/moves.md` stays as the Phase 3 move
ledger, history only.

## What is recorded

| Section | Contents |
| --- | --- |
| `== exports` | the five names in the built DLL's PE export table, sorted |
| `== marker` | `SEDICI_PROXY_V1 present` or `MISSING` (the deploy tools refuse a DLL without it) |
| `== binaries` | per test binary, `N passed; M failed; K ignored` from `cargo test`, and the total |
| `== verdicts` | one `binary test-name ok/FAILED/ignored` line per test |
| `== figures` | the Stage 1 PSNR/SSIM, HUD preservation and 10-bit equivalence lines, the present-queue counts, `SKIP`/`NOTE` lines; absolute memory sizes are recorded as "measured", not compared |

Timing is dropped (`finished in …`, `stage1 … ms`). Nothing in the file holds a host path, so two
checkouts compare.

## Reproduce

```powershell
cargo build --release                       # the image sections read target\release
pwsh tools/baseline.ps1 -Out after.txt      # record (UTF-8, no BOM)
pwsh tools/baseline.ps1 -Against docs\dev\baseline.txt   # "identical" or the differing lines
```

`tools/baseline.ps1` runs `cargo test -- --test-threads=1 --nocapture` itself (the GPU tests share
one adapter and some present to it, so they are serialised) and builds the release DLL when it is
missing. `-SkipTests` records only the image; `-SkipImage` records only the suite. Its parsing rule
is the procedure at the end of [`docs/rust/test-parity.md`](../rust/test-parity.md): per-binary
`test result:` lines, test verdicts, the numeric figure lines, and `SKIP` treated as "this host
could not run it", exactly as the C++ suite's `SKIP_RETURN_CODE 77` cases were.

## When to re-record

Re-record only after a comparison shows that every previously passing line still passes and the
differences are the additions the change intended. Record the comparison in the commit. A change
from `SKIP` to a verdict, or the reverse, is worth explaining: it usually means an adapter appeared
or disappeared on the machine, not a behaviour change (K112).

The record was first taken on `experiment/rust` on 2026-11-10 after the C++ build was retired:
523 tests across 13 test binaries and the doc-test target, all passing; exports and marker as above.
That run had a working D3D12 adapter, so the GPU checks ran rather than skipped.
