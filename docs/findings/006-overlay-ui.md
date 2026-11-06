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

