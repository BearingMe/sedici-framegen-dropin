# Dear ImGui

Vendored, unmodified Dear ImGui **v1.93.0** (MIT).

- Upstream: https://github.com/ocornut/imgui
- Release: https://github.com/ocornut/imgui/releases/tag/v1.93.0
- Tag commit: `c1aaac65e896a45f81f47a4d68f9959e0f86c859`
- Source archive: `https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.93.0`
- Archive SHA-256: `e21907fd8dd5ea59aa00538632a02ad0a3ededb4ddc533b94702331fbcd6c470`

Files are copied byte-for-byte from the release archive. Nothing here is
patched. `LICENSE.txt` is upstream's MIT license and applies to every file in
this directory.

Vendored:

- root: `imgui*.cpp`, `imgui*.h`, `imconfig.h`, `imstb_*.h`, `imgui_demo.cpp`
  (`imgui_demo.cpp` is kept for completeness and is not compiled)
- `backends/`: `imgui_impl_dx12.cpp`, `imgui_impl_dx12.h`,
  `imgui_impl_win32.cpp`, `imgui_impl_win32.h`

These files are not yet compiled or used by Sedici. The overlay renderer and
input handling have not been implemented.
