# Notices

Sedici stands on other people's work, and this file says so plainly. It lists what
we use, who made it, and the terms it comes with. If you redistribute Sedici, keep
this file with it.

## AMD FidelityFX SDK

Sedici is a proxy for AMD's FSR 3.1 frame-generation runtime and builds on AMD's
public API.

- **Headers.** `third_party/ffx/` holds unmodified headers from AMD FidelityFX SDK
  v2.4.0. Each file carries AMD's own notice, and `third_party/ffx/README.md`
  records the upstream path and blob ID of every header.
- **Runtime.** Sedici forwards to AMD's signed
  `amd_fidelityfx_framegeneration_dx12.dll`, which it loads as the `.orig` file
  next to itself. AMD's DLL is not part of this repository. A Sedici package that
  includes it ships it unmodified, with AMD's signature intact, and Sedici checks
  the file against a pinned SHA-256 before using it.

AMD publishes the SDK, including its signed binaries, under the following terms
(from `docs/license.md` in the FidelityFX SDK repository,
<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>):

> Copyright (C) Advanced Micro Devices, Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of
> this software and associated documentation files (the "Software"), to deal in the
> Software without restriction, including without limitation the rights to use,
> copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
> Software, and to permit persons to whom the Software is furnished to do so,
> subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
> FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
> AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
> WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

The same file also states a binary-only redistribution licence for files it does
not list. It requires that redistributions reproduce AMD's copyright, permission
notice and disclaimers, and it forbids reverse engineering, decompilation and
disassembly. Sedici does none of those things to AMD's binary. It loads the DLL,
calls its five public exports, and reads its file properties. The disclaimers
above apply to AMD's DLL in full.

AMD, FidelityFX, FSR and Radeon are trademarks of Advanced Micro Devices, Inc.
Sedici is an independent project. It is not made, endorsed or supported by AMD.

> **Responsibility.** The project owner has reviewed AMD's terms and accepts
> responsibility for redistributing AMD's DLL under them. This reading of the
> licence is the owner's, not legal advice.

## Third-party code in this repository

| Component | Where | Licence | Notes |
| --- | --- | --- | --- |
| toml++ v3.4.0, Mark Gillard | `third_party/tomlplusplus/` | MIT | Unmodified single header |
| Dear ImGui v1.93.0, Omar Cornut and contributors | `third_party/imgui/` (licence in `LICENSE.txt` there) | MIT | Unmodified, with its DX12 and Win32 backends |

## Research and reference material

- **Mob-FGSR** (`references/papers/mob-fgsr.md`, `references/sources/mobfgsr`).
  Sedici's stage-1 design follows this work. It is released under
  **CC BY-NC 4.0**, which does not allow commercial use. Sedici does not vendor
  its code. Any adaptation must record the origin commit and keep the
  attribution.
- **GFFE and STSSNet** (`references/papers/gffe.md`, `references/sources/stssnet`),
  plus the `tbfe`, `lmv` and `moflow` reference sources. They are read-only
  references and are never compiled into the DLL. Each keeps its own licence.

Thank you to AMD for publishing the FidelityFX SDK and its API in the open, and to
the authors above for sharing their work.
