# 011: P2.3 AMD binary evidence, decision and open gaps

Evidence record for P2.3. The full artifact table, runtime tables and the 8-point
integration contract are in [amd-binary.md](../refactor/amd-binary.md); this file
records the decision and what remains unproven. Nothing here ran in the game.

## Decision

| Question | Decision |
| --- | --- |
| AMD backend technically feasible | GO, using the installed copy pinned by SHA-256 `02297BEE...2F18` (FG 4.0.1.2740, providers 3.1.6 / 3.1.7) |
| AMD feeds Sedici's presenter | NO. AMD flips on its own thread; two backends, never two schedulers |
| Seamless mid-session handoff | NO. Takes effect at next swapchain recreation or an explicit quiesce |
| Bundling AMD's DLL (P2.9) | BLOCKED on a licence review a human must do |

## Facts the probe established

- Wrap create works only with a bare descriptor and consumes the caller's native
  chain reference. `ffxConfigure(FG)` goes to the FG context.
- Generation callback: once per game Present. Present callback: twice per frame, on
  AMD's thread. AMD owns presenting. Destruction order is free.
- Full destroy and recreate in one process works. A quiesced FG-only recreate works.
  Destroying only the FG context while presenting crashes.
- AMD does not validate its inputs: a null `gameQueue` crashes it and a 0x0 display
  size aborts the process, so Sedici must validate before forwarding.

## Open gaps

| Gap | Why it is open | Closed in |
| --- | --- | --- |
| Licence to redistribute the DLL | Terms are not in the file; needs a human to read AMD's FidelityFX SDK licence | Before P2.9 ships the DLL |
| Production-size inputs and cadence | The probe uses a 640x360 synthetic scene | P2.9 / P2.11 acceptance |
| Overlay composed through AMD's present callback | The probe's callback only copies | P2.8 / P2.9 |
| Any binary other than the pinned hash | Only one build was run; a different hash needs the probe re-run before enabling | Each AMD update |
| Hostile-input behaviour as a regression | Null queue (9), FG-only destroy while presenting (16) and 0x0 display (128) are observation variants that crash or abort AMD; they document the need to validate, they do not test Sedici's validation | P2.4 (validation boundary) |
| Retry wrap after a refused wrap | Variant 8 returned rc=3 and was never explained; dropped from the probe | P2.4 if it matters to recovery |
| Automated runs | The probe needs the user's AMD binary, so its CTest entry exists only with `SEDICI_AMD_FG_BINARY` set and skips otherwise; the agent classifier blocked running AMD's DLL, so the user ran it | Standing limitation |

Estimate changes: P2.4 must include the argument-validation boundary; P2.8 is simpler
(AMD keeps composition and the overlay attaches through the present callback); P2.9
shrinks to "load the installed copy by hash" plus a separate blocked "ship the DLL".
