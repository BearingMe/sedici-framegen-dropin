# Phase 4 triage (P4.1)

**Date:** 2026-11-09. One row per entry that [known-issues.md](../archive/known-issues.md) still showed as open, plus the
requests. Dispositions: **fix** (in the named milestone), **accept** (stays open by design, with
the reason), **obsolete** (evidence), **owner** (needs a decision). "Where" says whether the
fix can be proven offline (CTest, `sedici_fgcheck`, `sedici_regression`, ABI scenarios) or needs
the game. An entry with both halves is fixed and tested offline first and then listed in the
game checklist of its later milestone.

Status changes made by this triage: K21 and K32 are obsolete (the AMD path is gone, d95d454);
K103's status text is corrected to fixed (it was already counted as fixed; the contract text is in
`swapchain.h`); K59 stays partial. Counts after the triage: 46 open, 77 fixed or resolved, 12
obsolete, 1 retracted (136 entries).

## Defects

| Entry | Disposition | Milestone | Where | Note |
| --- | --- | --- | --- | --- |
| K1 | resolved by removal (2026-11-12) | P4.6 | game | Adaptive was measured in QA-18 step 3 and removed by the owner (issue #1, closed); QA-18 step 3 now measures 2x at ~32 fps. Remaining pacing work: #36, #39 |
| K2 | fix or accept | P4.6 | game | Generation cost feeding back into the real rate |
| K3 | fix | P4.2 | offline | Solved by the single target fps (FR-05): the planned output never exceeds the cap |
| K4 | fix or accept | P4.6 | game | FG off and uncapped still goes through the presenter; the target fps `0` default keeps this |
| K5 | investigate | P4.6 | game | VRR flips late with the guard off |
| K6 | accept with derivation | P4.6 | offline then game | Constants are tuned; write down the derivation, keep the fake-time tests |
| K7 | fix or accept | P4.6 | offline | Presenter drops a real frame when a later output is due |
| K8 | investigate | P4.6 | game | Frame-latency waitable object signalling |
| K9 | fix | P4.6 | offline | vsync-path XFAILs in `sedici_fgcheck --present` |
| K10 | fix | P4.6 | offline then game | Moved from P4.2: diagnostic wording and thresholds belong with the presenter work |
| K11 | fix | P4.3 | offline | Native chain recreation drops colour space and HDR metadata; failure leaves the game without a chain |
| K12 | verify | P4.3 | offline | Suspected, no failing run: reference handover in vsync/game |
| K13 | accept | P4.6 | game | RTSS limit is by design; keep the fallback |
| K14 | fix | P4.8 | offline build | 10-bit back buffer belongs to the Alan Wake 2 milestone |
| K15 | measure | P4.4 | offline | Snapshot ring and generated-image slots at production resolution |
| K16 | accept | - | - | Overlay counters include dropped outputs; revisit with K125 |
| K17 | fix | P4.2 | offline | First overlay log line prints device 0 and size 0 |
| K19 | fix | P4.2 | offline | Folded into the target fps `auto` rule: re-resolved when the refresh changes |
| K20 | fix | P4.3 | offline | Hidden env switches: document, move to config or remove |
| K21 | obsolete | - | - | The shipped toml and `config.h` no longer mention `async.mode` |
| K22 | fix | P4.2 | offline | Multiplier other than 2x in vsync/game is silently 2x |
| K23 | accept | - | - | Offline windows are not independent-flip; a limit of the test host |
| K24 | accept | - | - | RTSS in test processes; close it |
| K32 | obsolete | - | - | AMD probe variant, removed |
| K36 | accept | - | - | Full-menu click path cannot be driven unattended; game checklist |
| K54 | accept | - | - | Intermittent timeout, same load sensitivity as K112; investigated only if it recurs on an idle machine |
| K59 | fix | P4.3 | offline | `settings_snapshot` and `cancel` lock a mutex inside `noexcept`; add allocation fault injection |
| K98 | fix | P4.3 | offline then game | Slot-in-flight rejection returns an error to the game after resizes |
| K99 | fix | P4.3 | offline | Nothing feeds `stats_on_present` and `stats_on_swapchain_event` |
| K100 | fix | P4.8 | offline build, game | Alan Wake 2 |
| K101 | fix | P4.3 | offline | Stamp build identity in the log header and start a fresh log per deploy |
| K112 | accept | - | - | Load-sensitive assertions; acceptance runs on an idle machine |
| K117 | fix | P4.4 | offline | Per-pass timestamps, then optimise or rebase the budget |
| K118 | fix | P4.2 | offline then game | Slider; with FR-01, FR-02 and FR-05 |
| K119 | fix | P4.2 | offline then game | Offline half: persisted state and start-up path |
| K120 | fix | P4.4 | offline then game | Fast-motion scene in `sedici_fgcheck`; owner capture later |
| K121 | instrument | P4.3 / P4.7 | offline then game | Offline: log the time spent in each export; game: frame-time capture |
| K122 | instrument | P4.3 / P4.7 | offline then game | As K121, for create, configure and destroy on a resize |
| K123 | fix or accept | P4.7 | game | Slow settle of the real rate |
| K124 | fix | P4.2 | offline | Log the exception; contain it; shutdown-order test |
| K125 | fix | P4.2 | offline | Show shown and dropped real frames |
| K126 | fix | P4.2 | offline then game | Offline: key-routing logic passes Alt+F4; game: retest |
| K127 | fix | P4.3 | offline then game | Bounded wait for retirement on destroy |
| K128 | investigate | P4.7 | game | Loading-screen borders |
| K129 | decide | P4.8 | game | Owner decision, taken when that milestone's call list exists |
| K130 | fix | P4.8 | offline build, game | Device-object mismatch |
| K131 | fix | P4.8 | offline build, game | Interpolation queries |
| K132 | fix | P4.8 | game | Cursor interception in Alan Wake 2 |

## Requests

| Request | Disposition | Milestone |
| --- | --- | --- |
| FR-01, FR-02 | do | P4.2 |
| FR-03 | out of Phase 4 (`tools/intro_skip.ps1`) | - |
| FR-04 | do (accepted) | P4.2 |
| FR-05 | do (decided: one target fps) | P4.2 |
