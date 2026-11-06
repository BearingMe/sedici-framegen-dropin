# 006: Overlay callback investigation

## Status

The composition-safe Step 0 callback is implemented and awaiting the user's
in-game visual check. The first prototype replaced the runtime's image
composition without writing the output image. The user observed a frozen CDPR
logo while menu audio continued. Disabling the callback restored the picture,
as confirmed by the user. That is strong evidence of the defect, not a
controlled GPU-level test.

The replacement only takes over when no separate UI resource is registered,
which is Cyberpunk's configuration. In that case the required operation is
exactly the back-buffer copy AMD's default callback performs: transition the
current back buffer to `COPY_SOURCE`, transition the output to `COPY_DEST`,
`CopyResource`, then restore both incoming states. When a UI resource is
registered, Sedici leaves the callback null so FFX keeps its shader-based
composition. If the copy cannot be performed, Sedici records the failure and
leaves the next configure to FFX.

## Verification

- An 8x8 DX12 pixel test fills a real source texture with a known pattern,
  runs the copy with `FFX_API_RESOURCE_STATE_PRESENT` on both resources, reads
  the output back and compares every pixel. It passes.
- A three-scenario proxy test confirms the patching decision: no UI/no game
  callback installs Sedici's copy callback; a non-empty UI resource leaves the
  callback null; a game callback is chained and the caller's desc is restored.
- The existing 56-check harness passes and the DLL still has exactly five
  exports.


## Step 0 observations

In the affected run, the callback received a command list and output resource;
its first logged invocation reported `currentUI` absent and output resource
state `128` (`FFX_API_RESOURCE_STATE_PRESENT`). Each of 102 logged windows
contained 150 real and 150 generated callback invocations. Callback invocation
rate does not establish physical display cadence or prove that the output image
was populated. The runtime did not report a callback error.

The installed original DLL exports only the five `ffx*` functions. In
particular, it does not export the default UI-composition callback for Sedici
to invoke through the public DLL interface.

AMD's public [DX12 swapchain source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.cpp)
selects a supplied present callback in place of its default. Its
[default composition implementation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_UiComposition.cpp)
copies the current back buffer to the output if no separate UI exists, and
uses a rendering pass when a UI resource is present. These links refer to SDK
main, not necessarily the exact source for the installed binary.

The previous synthetic harness verified invocation and chaining but did not
validate any GPU commands or resulting pixels. It cannot establish that the
callback is safe.

## Remaining gate

Ask the user to verify logo, menu and gameplay with frame generation on, and
confirm there is no frozen image or visual change. Only then proceed to draw
the overlay after the copy, and to implement the Mini/Full UI.

The separate-UI path remains FFX's own and cannot host Sedici's overlay. That
is acceptable for Cyberpunk, which reports `uiResource=no`; document it rather
than silently dropping UI composition.

Until the visual check passes, retain the working DLL and leave the overlay
renderer disabled. Dear ImGui remains vendored but is not compiled or used.

## Milestone 3 implementation

- Dear ImGui v1.93.0 with the DX12 and Win32 backends. Rendering happens on the
  FFX present callback's command list after Sedici's composition copy, so it
  covers both real and generated output frames.
- `Alt+F10` cycles `Off -> Mini -> Full -> Off`. The input hook is installed with
  the game window even while the menu is off; ImGui itself is not initialized
  until a visible mode is requested.
- Mini is one line. Full contains status, real/output graphs, the live FPS limit,
  pacing, async, log level, menu scale, and Save/Cancel/Close.
- Live values live in `src/live.*`; the limiter resets its schedule on change,
  pacing re-sends to tracked swapchain contexts, async changes future configure
  descs, and logging reads the new level immediately.
- Save uses `src/config_patch.*`, a line-oriented patcher that preserves
  comments, unrelated keys and line endings and appends missing keys/sections.
  It does not rewrite `[ui] mode`; that remains a startup preference.
- Controls overridden by `SEDICI_*`/`MFSRFG_LOG` are disabled in the menu.

## Offline verification

- A direct DX12 test renders Mini and Full over a copied 1280x720 image and
  reads the output back. Both pass; the background copy remains intact outside
  the menu. The Mini frame changed 3,515 pixels in the sampled region; Full
  changed 52,220.
- A synthetic `Alt+F10` WndProc test cycles `Off -> Mini -> Full -> Off`.
- The config-patch test verifies comments, CRLF, sibling keys, an existing key
  with a trailing comment, and a missing `[log]` section.
- The existing 56-check harness passes; exactly five exports.
- 32-bit output is not exercised by these tests; the game test covers the real
  swapchain and the actual input path.

## In-game verification pending

- `Alt+F10` cycling with the game focused.
- Cursor and mouse/keyboard capture in Full; game input must not leak through.
- Live limit `36.5 -> 37 -> off`, pacing on/off, async, log level, scale.
- Save/Cancel/Close and restart persistence.
- No ghosting of the menu, and no frozen output.
- One PresentMon capture with Mini and the limiter at 37, compared with the
  same build at UI Off and with historical run G.

## Cursor capture follow-up

The user reported that the Full menu cursor remained at screen center while the
game kept recentering it. `dumpbin /imports` shows `SetCursorPos`, `ClipCursor`
and `GetCursorPos` in `Cyberpunk2077.exe`, so the imports are an available call
path. Sedici now intercepts those two slots in the executable's import table:

- Only while Full is requested, the game window is foreground, and the overlay
  has not failed. Off and Mini pass every call through untouched.
- `SetCursorPos` is acknowledged and dropped, so a recenter cannot move the
  cursor. `ClipCursor` is recorded instead of applied, so confinement cannot
  trap the cursor in the middle.
- On entry the current clip rectangle is saved; `ClipCursor(nullptr)` releases
  confinement for the desktop. On exit the last rectangle the game requested
  during capture is restored, so normal mouse look returns.
- Focus loss, window destruction, window replacement, overlay failure and mode
  changes all release capture. `ShowCursor` adjustments are counted so the
  original visibility is restored exactly.
- If the import slots cannot be patched (different module, dynamically resolved
  API), Full falls back to Mini with a log line rather than consuming input.

Caveat: only the executable's import slots are patched. Calls made by another
loaded module through its own thunk, or resolved with `GetProcAddress`, bypass
interception and are not covered. If the in-game report persists after this
change, the next step is to identify the module that owns the call rather than
broadening interception blindly.

### Cursor test results

`cursor_test.cpp` builds as its own executable so the interception targets its
own imports, exactly as it targets the game's. All 16 checks pass:

- mapping a 640x360 client point to a 1280x720 output, clamping, and the
  no-size fallback;
- pass-through of `SetCursorPos` and `ClipCursor` before capture;
- suppression of both while capturing;
- restoration of the last requested clip on exit, and cursor movement again;
- double leave, re-entry, and focus-loss release.

The DX12 Mini/Full rendering tests, the synthetic hotkey cycle, the 56-check
harness and the five-export check still pass.

## Review hardening

- Bind the shader-visible SRV heap before ImGui draws. Validate device-object
  creation explicitly, keep backend assertions active as catchable failures in
  Release, and contain backend access violations at narrow DX12 call boundaries.
  This does not make asynchronous GPU faults recoverable.
- Keep each window's original procedure until `WM_NCDESTROY` has been forwarded.
  Do not reinstall above another overlay's subclass; rebind the Win32 backend
  when the output HWND changes.
- A permanent overlay failure disables hotkeys and cursor suppression immediately;
  visibility/clip cleanup is posted to the owning window thread. Cursor counters
  are atomic and capture transitions are serialized. Full is also required for
  cursor-call suppression, including the interval before queued cleanup runs.
- Remove allocating helpers' `noexcept` boundaries so the outer handlers can
  actually catch allocation/filesystem failures.
- Install Present timing even with startup logging/limiter off, so the live
  limiter can work later. Each patched vtable retains its own originals.
- Give each callback/user-context pair an immutable, bounded, process-lifetime
  binding. Rejected configure calls do not publish composition registrations.
- Queue live pacing sends until application Present, outside FFX's callback;
  serialize sends against context destruction and recheck tracked contexts there.
- Respect TOML case and array-table boundaries; parse and compare the entire
  patched document against the intended semantic result before replacing the
  original. Unsupported layouts fail without changing the file.
- Decode mouse coordinates as signed and update mapping dimensions before
  consuming input. Serialize access to the shared ImGui renderer.

`SEDICI_BUILD_TESTS=ON` adds repository-owned regression and Mini/Full DX12 tests.
The GPU tests submit twelve frames and replace the window halfway through, using
the installed DX12 debug layer when available. They fence each test submission:
passing them is **not** evidence that FFX bounds outstanding callback submissions
to ImGui's four upload-buffer slots. That in-game lifetime assumption remains
unverified; increasing the slot count would not prove correctness either.
