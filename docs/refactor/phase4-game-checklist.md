# Phase 4 game checklist (P4.6 to P4.9)



What is ready for the owner's sessions and how each result becomes a verdict. Everything an agent

could do without the game is done: the code changes, the instrumentation, the analyzers and the QA

entries below. Nothing here has run in a game.



Rules for the sessions are in [tests/qa/README.md](../../tests/qa/README.md): deploy with

`tools/deploy.ps1`, **delete `sedici.log` before every run**, `[log] level = 1`, copy the log into the

test folder right after the run. Two tools turn the raw files into numbers (both read only):



```powershell

.\tools\analyze_log.ps1      -Log <sedici.log>                       # rejections, stalls, settle time, events

.\tools\analyze_presents.ps1 -Csv <capture.csv> -TargetFps 60        # cadence, percentiles, 1% low, stalls

```



A capture is PresentMon or CapFrameX output (any CSV with a `MsBetweenPresents` or `FrameTime`

column). Paste the analyzer output into the QA folder's README next to the raw evidence.



## What changed in the build under test



| Change | Entries | Where to look in the log |

| --- | --- | --- |

| `SwapChainNewDX12` and `ForHwndDX12` are answered; `R10G10B10A2_UNORM` and the HDR create flag are accepted; the interpolation list and texture queries are answered; the game's own generation dispatch is acknowledged | K14, K100, K129, K131 | `created swapchain (new)`; `FrameGenerationCreate ... result=0`; `interpolation ... answered` at log level 2 |

| A chain device that differs from the queue's device is accepted on the same adapter | K130 | `chain device ... differs from queue device ...; same adapter, using the queue's device` |

| Cursor interception scans the executable and every non-system module, rescans every two seconds, and says what it found | K132 | `overlay cursor interception installed/unavailable: modules=N slots{SetCursorPos=a ClipCursor=b} skipped=s seen{...}` |

| After every swapchain create or resize the overlay reports every 60 outputs for eight reports, then every 300 | K123 | several `overlay present frames=60 ...` lines after a change |

| Prepare slot wait 250 ms; retirement wait 1000 ms; `slow call` lines; stable `[code=E_...]` | K98, K127, K121, K122 | `rejected: ... [code=E_SLOT_BUSY]`, `fg: slow call op=...` |

| Single target fps (`fg.target_fps`), Alt+F4 passes through the Full menu | K3, K118, K126 | `effective:` lines, QA-13 |



## Verdict rules



A verdict is one of: **fixed** (criterion met), **accepted** (written reason in the entry), or **open**

with a cause recorded. The criterion for each entry is what the analyzer prints.



### P4.6 Pacing and presentation (QA-18, and the cadence part of QA-04, QA-05, QA-10)



| Entry | Run | Criterion | If it fails |

| --- | --- | --- | --- |

| K1 adaptive latency and late flips (issue #1) | QA-18 step 3 (adaptive at about 32 real fps, `[log] level = 2`) | presented output at least 95% of the adaptive target (73 fps at 75 Hz: 69.4 or more); `planned_output_fps` at least 99% of the target (72.3 or more); `late` below 5% of flips in the `vrr present` lines. Changed 2026-11-12 with the owner's approval: adaptive aims at the auto target (`floor(hz - hz^2/3600)`, 73 at 75 Hz), not the display rate | keep open, attach the capture and the `vrr trace` lines |

| K2 generation load lowers the real rate | QA-18 step 2 (2x, then FG off, same scene) | real fps with FG on within 10% of FG off (the cost of generation, stated) | record the cost in ms from the two runs; decide with the owner whether a budget is needed |

| K4 FG off throttles the game | QA-18 step 4 (menu, FG idle, game uncapped) | frame time with Sedici within 5% of the same menu without Sedici (`tools/restore.ps1`) | accept as inherent only if both throttle to the same rate |

| K5 flips 10.3/16.9 ms apart | QA-18 step 5 (`tear_guard = "off"`, VRR) | flip spacing from `vrr present` lines; record, there is no pass rule: this is a driver observation | accepted as a driver behaviour, with the numbers |

| K7 real frames dropped by the presenter | QA-18 step 2 | `dropped` real frames below 1% of real frames while output is below the refresh | open, with the share |

| K8 frame-latency waitable | QA-18 step 6 | game frame time stable (99th percentile within 1.5x the median), vrr compared with vsync | open with the numbers |

| K9 vsync path XFAILs | none (offline, guarded by `sedici_fgcheck --present` XFAIL) | stays XFAIL; QA-09 under RTSS shows whether it matters in play | |

| K15 cost of the vrr snapshot copy | QA-18 step 2 vs the same run with `present = "vsync"` | difference in GPU time below 0.1 ms at 1080p | open with the number |



K6 (tuned constants) is accepted now as tuned constants with game evidence: the decisions that use them

(`presentation/pacing.cpp`, `presenter.cpp`) are pure functions checked by the fake-time cases in

`fgcheck_pacing.cpp`, some constants are still literals in the code, and any change to one must come

from a QA-18 capture, not from a derivation.



### P4.7 In-game defects (QA-19, QA-20)



| Entry | Run | Criterion |

| --- | --- | --- |

| K98 prepare-slot rejections after window changes | QA-19 | `prepareSlotRejections = 0` over the whole run |

| K122 gaps after mode changes | QA-19 | every gap of a second or more is listed by the analyzer; a `slow call` line with the same time means Sedici held the thread, none means the game did |

| K123 slow settle of the real rate | QA-19 | the new early lines show the real fps per 60 outputs: the analyzer prints the settle time; criterion 3 s or less after each create |

| K127 / K126 exit | QA-13 retest inside QA-19 | `destroyRefusals = 0`; Alt+F4 with Full open closes the game |

| K121 short stalls | QA-20 step 4 | the analyzer's stall list with `slow call` lines: stalls with no slow call are the game's |

| K119 overlay dead at the next launch | QA-20 step 3 | keep `sedici.toml` and the full log of the failing launch; the `overlay` and `interception` lines say which part failed |

| K124 overlay exception | any run | the log now carries the exception text; none expected |

| K120 disocclusion at speed | QA-20 step 1 | a capture exists (video or screenshot pair) with the time in the run |

| K128 borders on the loading screen | QA-20 step 2 | a capture exists with the time in the run |

| K36 Full-menu clicks at another resolution | QA-19 step 4 | clicks land where the cursor is at 1920x1080 on a 2560x1080 desktop |



### P4.8 Alan Wake 2 (QA-03, QA-17)



| Entry | Criterion |

| --- | --- |

| K100 route | the create is accepted: `created swapchain (new)` and the game reaches its menu |

| K14 10-bit | frames present; no `unsupported` line |

| K129 HDR flag | `FrameGenerationCreate ... result=0`; generated frames visible; with the in-game HDR on and off the generated frames have the same brightness and colour as the real ones (a side-by-side screenshot of a still scene with the camera moving slowly) |

| K130 device objects | steady play for five minutes and a resize without a device-removed line or a `DX12 error` |

| K131 interpolation shim | no `interpolation ... unavailable` line; game does not reject the swapchain |

| K132 Full menu | `overlay cursor interception installed ...` with `SetCursorPos` and `ClipCursor` slots at least 1, and the Full menu opens; if `unavailable`, the `seen{...}` field says which import the game lacks |



If Alan Wake 2 behaves differently from what finding 015 saw (for example it never dispatches its

own generation, or calls something not yet seen), the log lists every call: keep it, it is the finding.



### P4.9 Final acceptance



All QA entries with a Result, `tools/baseline.ps1 -Against docs/dev/baseline.txt` identical on the

final build, counts and tables current, owner sign-off recorded in `MILESTONES.md`.

