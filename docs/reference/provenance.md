# Requirements and provenance

> Reference moved out of the [README](../../README.md); the text is unchanged.

## Requirements

- Windows x64, Visual Studio 2022 C++ tools, CMake 3.21+, Windows SDK.
- Static CRT, C++20.
- The FidelityFX/FSR headers in `third_party/ffx/` are unmodified SDK v2.4.0
  headers. See `third_party/ffx/README.md` for upstream paths and blob IDs.
- toml++ v3.4.0, single-header flavour, unmodified, in
  `third_party/tomlplusplus/toml.hpp`. MIT.
- Dear ImGui v1.93.0, unmodified, in `third_party/imgui/` with the DX12 and
  Win32 backends. MIT. See `third_party/imgui/README.md`.

No other dependencies.

### Dear ImGui provenance

| | |
|---|---|
| Project | [ocornut/imgui](https://github.com/ocornut/imgui) |
| Version | **v1.93.0** (released 2026-10-14) |
| Tag commit | `c1aaac65e896a45f81f47a4d68f9959e0f86c859` |
| Sources | core `imgui*.cpp/.h`, `imconfig.h`, `imstb_*.h`; `backends/imgui_impl_dx12.*`, `backends/imgui_impl_win32.*` |
| Licence | MIT (`third_party/imgui/LICENSE.txt`) |

The files are copied byte-for-byte from the release archive. `imgui_demo.cpp`
is vendored for completeness and is not compiled.

### toml++ provenance

| | |
|---|---|
| Project | [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) |
| Version | **v3.4.0** (tag `v3.4.0`, released 2023-10-13) |
| File | single-header `toml.hpp`, 485,931 bytes |
| SHA-256 | `6B5172AD4DD6519AEC67B919181FA7A38A2234131E5B2AFA232DFE444819783E` |
| Licence | MIT (`SPDX-License-Identifier: MIT` in the header preamble) |
| Source | `https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp` |

The file is vendored byte-for-byte; nothing in it is edited. Two documented
compile-time switches are set in `src/settings/toml_include.h` before the include, not in the
header, and every unit includes toml++ only through that file so the types agree across
translation units:

- `TOML_EXCEPTIONS=0` so `toml::parse` returns a `parse_result` instead of
  throwing. Nothing about configuration parsing may throw across an `ffx*`
  export boundary.
- `TOML_ENABLE_FORMATTERS=0` because Sedici only parses; this drops the
  serialisers.

`CMakeLists.txt` fails the configure step with an explanatory message if the
header is missing.
