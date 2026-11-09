# 013: P2.5 GPU submission and retirement

> **Update (2026-11-09):** K37, K41 and K48 were resolved after this record (cf48198,
> 721546a). The unbounded-retention remainder is K47 and the P2.10 watchdog gap.

Records the retirement audit, the defects it found, how they are covered and what is
left. Nothing here has run in the game.

**Correction:** P2.5 is not complete. The timeout-retention approach below has safety and boundedness defects (K45–K48); its original unconditional safety claims and device-loss test characterization are superseded by this correction. The test does not prove generation was in flight or that the post-loss Present failed.

## What the audit checked

Every place that waits on or releases GPU-visible state: `Context::retired`,
`wait_for_retirement` and `destroy`; `Swapchain::drain`, `wait_slot`, `resize` and the
destructor; `Presenter::wait_fence` and `drain_gpu`; and the `ExecuteCommandLists`
followed by `Signal` sequences in the swapchain. The rule applied is the milestone's:
a timeout is not permission to free in-flight memory, and no completion is faked.

## Already correct

- Resize refuses when the drain times out, before any buffer is released.
- Bounded waits (5 s swapchain, 200 ms session) never CPU-signal a fence.
- Each frame slot's lists and images are reused only after that slot's fence completes.
- Multiple frames in flight, destroy with work outstanding and resize are covered by the
  `sedici_abi` scenarios `destroy-inflight` and `window-replace` (P2.2), with the debug
  layer counting errors.

## Defects found and fixed

| ID | Defect | Fix |
| --- | --- | --- |
| K38 | A removed device left `Context::retired()` false forever, so destroy was refused permanently | A removed device counts as retired. `sedici_fgcheck --device-loss` removes the device after generation is submitted and tracked, and checks that presenting does not hang, that the session destroys, and that teardown takes under 3 s. It fails without the fix. |
| K39 | `~Swapchain` freed all GPU objects after a timed-out drain on a live device | All GPU objects and the presenter are retained for the process lifetime and logged |
| K40 | A refused `Session::destroy` left the session usable and the owner was freed anyway | The session stays Draining; the owner is parked, reaped on later calls once it retires, and never freed while work is in flight. The parked list is leaked deliberately at process exit. |

Retention has no fixed bound and is not exception-safe; a leaked presenter may continue callbacks (K47, K48). The count logged for parked sessions is not a bound.

## Not done, with owner

| Item | Why | Owner |
| --- | --- | --- |
| K37: `Signal` failing after `ExecuteCommandLists` on a live device | Not injectable without a device or queue abstraction | P2.5 follow-up |
| K41: tests for the retention paths (K39, K40) | A live GPU cannot be stalled on demand | P2.5 follow-up, P2.10 watchdog |
| Per-slot descriptor ranges and scratch | The stage uses fixed objects, not descriptor ranges that move between slots, so there is no descriptor retirement to make explicit yet | Revisit when stage 2 adds inference scratch |
| A device and queue abstraction for fault injection | Larger design change; the device-removal test covers the loss case without it | Decide after P2.7 |

## Owner acceptance (2026-11-08)

The owner accepted P2.5 with recorded gaps. Added since the correction above: K104 (destroy wait releases the context lock, tested with a mutation), K105 (schedule restored after a refused presenter job, fault-injected) and a live-GPU stall test for the swapchain retention path (`--retention-timeout`, K41). Open and accepted: K37 cross-queue work, K77, the router parking test, and unbounded timeout retention (K47, K48), which moves to the P2.10 watchdog.
