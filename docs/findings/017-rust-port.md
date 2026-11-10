# Finding 017: porting Sedici to Rust (experiment/rust)

Date: 2026-11-10. Branch `experiment/rust`, never merged to `main`. Design and port map:
`docs/rust/port-design.md`. Test mapping: `docs/rust/test-parity.md`. Bugs: K141 to K178 in
`docs/refactor/known-issues.md` (the ones marked "Rust port only").

This note is for the C++ code. The port is an experiment; what is worth keeping is what it showed
about the C++ design, its tests and the way the work was done.

## 1. Outcome

- Every C++ source under `src/` and every offline test under `tests/` has a Rust equivalent, and the
  C++ build was retired on the branch (CMake, `*.cpp`, `*.h`, vendored ImGui and toml++ removed;
  `third_party/ffx` kept as reference headers).
- Verified offline: `cargo build --release` ok; the DLL exports exactly the five `ffx*` names (read
  from the PE export table); `cargo test -- --test-threads=1` 522 passed, 0 failed, 14 binaries;
  layering and `unsafe` containment enforced by `tests/architecture.rs`; Stage 1 reproduces the C++
  quality numbers on the real GPU (42.92 / 40.60 / 45.99 dB).
- **Not validated in a game, and the first real run failed.** The owner deployed the Rust build to
  Cyberpunk 2077 and the game crashed. `sedici.log` ends right after
  `ffxCreateContext #2 ... FrameGenerationSwapChainWrapDX12 variant=wrap`, before the first Present
  line. No dump, no Windows Application Error entry. Cause unknown (K178). The investigation was
  stopped by the owner before a reproduction existed, so there is no root cause here and none is
  claimed.
- Why the offline suite did not see it: Cyberpunk uses the **wrap** form (the game hands over a swapchain
  it already created and Sedici writes the wrapper back through a pointer slot). The C++ suite
  exercises that form (`abi.cpp`, `fgcheck_routing.cpp`; I did not check how much of it runs on a real
  chain). In Rust, `tests/routing.rs` covers it with a fake factory, and `tests/swapchain_smoke.rs` /
  `tests/lifecycle.rs` use the New and HWND forms through the real factory. So the one path the first game needed was the one path the Rust suite only
  ran against fakes. Parity by scenario name is not parity by what is exercised.

## 2. What the port says about the C++ code

None of the port's 37 bugs is a confirmed defect in the C++. The C++ was right where the port was
wrong (for example `Swapchain::interpolation_list` already closes the scratch list before resetting
its allocator, which the Rust port first forgot, K168). What follows are improvements the port made
visible, in order of value.

1. **Seams made the hard parts testable without a GPU.** The Rust presentation layer talks to the
   outside through five traits (`NativeSwapchain`, `GpuDevice`/queue/fence, `Clock`, `FrameGenerator`,
   `OverlayHook`). With fakes, `tests/presentation.rs` runs 39 cases (present admission, back-pressure,
   drain and retirement timeouts, removed device, the K138 lock split, the K125 real-frame drop rule,
   tear-guard arithmetic, K140 queue counting) in milliseconds, with nothing skipped. In C++ the same
   scenarios need a real adapter, a window and hand-written fault hooks (`bookkeeping_faults`,
   `routing_hook_slow`, `signal_then_wait` lambdas). Suggestion: give `Swapchain` and `Presenter` an
   abstract native-chain interface and an injectable clock, and replace the process-global fault
   hooks with fakes. A modelling fake also caught a D3D12 rule the real tests only hit by accident:
   a command allocator cannot be reset while a list recorded from it is open (K168).

2. **More of the logic can be pure.** `gpu/*.h`, `limiter_schedule.h`, `target_rate.h` and `ui_logic.h`
   already are. The port moved these to the same shape and they became trivially testable:
   - pacing plan and output schedule (`domain/pacing.rs`) with time passed in;
   - the rolling timing rings and percentiles;
   - config precedence with an environment lookup closure instead of `getenv`, so the whole
     default / file / env order is table-testable without touching the process environment;
   - the settings transaction state machine and the Full-menu view-model: `ui.cpp` (1149 lines) mixes
     ImGui calls with edit/apply/cancel logic; in Rust the menu is a plain view-model plus actions,
     and only the adapter knows the widget library.

3. **State the rules in code, not only in K entries.** Several rules were load-bearing and invisible
   until the port had to reproduce them:
   - `Context::prepare` negates the FFX motion-vector scale before Stage 1; the sign is required and
     nothing in the C++ says so. A comment and a one-line test would do.
   - The single prepare slot (K139, `E_SLOT_BUSY`) is the likely culprit for the Alan Wake 2 black
     screen; the port copied it unchanged on purpose and so carries the same problem.
   - The scratch interpolation list is deliberately never submitted (the swapchain submits it in AMD's
     design); only a K entry says so.
   - `DXGI_ERROR_INVALID_CALL` versus `E_INVALIDARG` after a presenter has stopped: the C++ answers the
     former, and the port got it wrong first (K167). The HRESULTs Sedici hands to a game are spread
     across `swapchain.cpp`, `presenter.cpp` and `route.cpp`; a single table (like `error_code.h` is
     for rejections) would make the contract checkable.

4. **Ownership of references should be written down once.** `Attach` versus `AddRef` at each hand-off
   (wrap desc, New desc, game queue, back buffers, the FFX resource handles) is the single most
   error-prone area of the port: K169 (adopted a game handle with an owning constructor, then added
   another reference: a leak and then a use-after-free on the second borrow, measured as an access
   violation) and K151 (the `Presenter::adopt` AddRef/Release probe has no safe equivalent). The C++
   follows a consistent rule, but only in code. A table "who owns which reference at each hand-off"
   in `docs/dev/` would catch the next mistake in review.

5. **D3D12 debug layer in the offline suites.** Of the renderer bugs found only one was visible
   without it: K155 (descriptor table set on the wrong root parameter) was reported solely by the debug
   layer; K154 (a zero `SampleMask` draws nothing) produced no error at all and was caught by reading
   pixels back. If the C++ offline GPU suites do not enable the debug layer (and GPU-based validation
   in a separate run), enable them; pixel read-back checks like the Rust overlay test are worth keeping
   for every pass that writes to a back buffer.

6. **Build identification (K101, still open).** The Rust log says `sedici: build config.rs 0.1.0`, which
   is no better than the C++'s compile-time stamp: it still cannot say which DLL ran. The C++ attempt to
   fix it (an always-stale object dependency) was rejected. A cleaner way for either language is a
   generated one-line source with `git describe --dirty` written at configure/build time (CMake
   `configure_file` with a custom command, or a `build.rs` setting `SEDICI_GIT_HASH`), and logging it
   as the first line. Every in-game run (QA-03, this crash) would then be attributable.

7. **Crash diagnostics are missing in both versions.** A hard crash leaves nothing but a truncated log.
   A vectored exception handler that writes one synchronous line (code, address, module+offset, last
   entered `ffx*` or COM method) and a flush at the end of `ffxCreateContext` would have turned the
   Cyberpunk crash from "the log stops" into an address. This was the second half of the stopped
   investigation. It is cheap, independent of the cause and worth doing in the C++ too (level-2 entry
   traces of the COM wrapper methods were part of the same plan).

8. **Layering checks can also pin `unsafe`.** `tests/architecture.rs` reads the sources and fails on a
   layer import that points up, and on `unsafe` anywhere outside `src/adapters`. The C++ analogue of
   the second rule is "raw `ID3D12*` / `HWND` only in these directories"; the first exists
   (`check_layers.ps1`).

## 3. Method notes

What worked:
- **Layout and stubs first.** The whole module tree was created as empty files with doc comments and
  one design document before any porting, so five parallel workers could each fill their own files
  and merge with almost no code conflicts.
- **Leaves first, core last.** Pure domain, then config, diagnostics, GPU and Stage 1, Win32, FFX ABI,
  then the application core against the ports the leaves had defined, then presentation, overlay,
  wiring, tests, retirement. Each merge ran the full suite.
- **Port the scenario, not the line.** Mapping every C++ scenario to a Rust test or a named GAP
  (`docs/rust/test-parity.md`) kept the test count modest (about 520) while exposing three real
  defects (K167 to K169) the first pass had missed.
- **Wrapper types around COM and Win32** gave one place for each `unsafe` block with a `SAFETY` note.

What cost time or hid risk:
- **Shared documents are the merge conflict.** `known-issues.md` and `MILESTONES.md` conflicted on every
  merge and K numbers collided between workers (K141 and K142 were claimed by three workers at once;
  eight renumberings in total, and code comments that cite K numbers had to be rewritten). Next time:
  reserve a K range per worker in the brief.
- **The `windows` crate is a different trap set**, all recorded: `Default` for D3D12 structs zeroes
  `SampleDesc.Count` and `SampleMask` (K147, K154), COM `_Impl` forwarders cannot be `unsafe fn` (K172),
  `cdylib` plus `rlib` makes `cargo test` unlinkable without the exports (K146), a custom `DllMain`
  adds a sixth export (K148), and a `#[no_mangle]` marker must be kept alive by hand under LTO (K170).
- **The export rule needed a test that cannot skip** (K171): the first export check skipped silently
  when the DLL had not been built.
- **Tooling coupled to the old build** (deploy source path, baseline recorder, layer script, deploy
  check) all needed repointing, and one new script had a case-insensitive variable collision (K177).
- **The auto-mode classifier** blocked a worker's report and the deploy command with "no reason
  provided". The owner added allow rules for `cargo` and ran the deploy themselves. The owner's reading
  is that the model gave no reason for the action; the classifier's own rule is not visible.
- **Shipping to a game before the wrap path was exercised with a real chain.** See section 1.

## 4. Gaps left on the Rust side

Recorded in `docs/rust/test-parity.md` and the K list; none is a regression of the C++:
the real limiter and present-hook wiring, swapchain parking and the reaper, DLL pin and unload (the
port has no module reference by design), end-to-end overlay hotkey, the K60 and K68 fault hooks, the
VRR wall-clock run, prepare statistics (K166), the overlay on 10-bit chains (K164), the HWND
`fullscreenDesc` field (K160), and `rust-version` 1.80 against egui's 1.88 (K173).

## 5. Recommendations

1. Treat `experiment/rust` as a reference and a source of test ideas, not as the next build: it has not
   survived a game launch. Do not deploy it again before a wrap-form test with a real DXGI chain
   reproduces and explains K178.
2. Take these into the C++ (independent of Rust): the build-identification line (section 2.6), the
   crash handler and flush (2.7), the debug layer plus pixel read-back in the GPU suites (2.5), a
   written reference-ownership table (2.4), and a comment plus test for the motion-scale sign (2.3).
3. For Alan Wake 2, the port adds no evidence beyond K139: its single prepare slot is unchanged, so a
   per-frame prepare ring remains the first thing to try (needs owner approval, see finding 016).
4. If the Rust work resumes: start by reproducing K178 offline (a Cyberpunk-shaped wrap: a chain made by
   `CreateSwapChainForHwnd`, wrapped through the exported `ffxCreateContext`, then QueryInterface,
   GetBuffer, Present, ResizeBuffers on the returned object), then the crash handler, then redeploy.
