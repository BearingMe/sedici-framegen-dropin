# Vendored FidelityFX/FSR headers

Unmodified headers from **AMD FidelityFX SDK v2.4.0** (FSR Frame Generation 4.0.2,
DX12 swapchain 4.0.0). The relative include paths are the upstream layout, so
do not move individual files without updating their includes.

| File | Upstream path |
|---|---|
| `api/include/ffx_api.h` | `Kits/FidelityFX/api/include/ffx_api.h` |
| `api/include/ffx_api_types.h` | `Kits/FidelityFX/api/include/ffx_api_types.h` |
| `api/include/dx12/ffx_api_dx12.h` | `Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h` |
| `framegeneration/include/ffx_framegeneration.h` | `Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h` |
| `framegeneration/include/ffx_framegeneration_api_types.h` | `Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h` |
| `framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` | `Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` |

Git blob IDs, used to verify the local copies against upstream:

```text
ffx_api.h                          d351c168edad86d0f7851bfd8ff5eb77be3667f0
ffx_api_types.h                    380c4c0a1bcc7eb9bb4097f1046de886f9f4c587
dx12/ffx_api_dx12.h                1b711807c597630f4a8d2dfa2572a845c069829a
ffx_framegeneration.h              823a3b4720f10e03c1fcf6db2a85d7c60d846eac
ffx_framegeneration_api_types.h    e7e514ddb6666753de3b14f720d8e6774339b0b9
dx12/ffx_api_framegeneration_dx12.h 908b300183983b78f839598c274e2cfc0fdd8651
```

These headers carry AMD's MIT notices. AMD runtime DLLs are not bundled here.
