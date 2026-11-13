# Call handling and return codes

> Reference moved out of the [README](../../README.md); the text is unchanged.

## How a call is answered

1. **Answer every call.** `ffxCreateContext` makes either Sedici's frame
   generation context or its swapchain wrapper. `ffxConfigure`, `ffxDispatch`,
   `ffxQuery` and `ffxDestroyContext` act on handles Sedici issued. Handles come
   from a reserved token namespace, so a destroyed or foreign handle is
   recognised and refused locally.
2. **Version queries.** `ffxQuery(GetVersions)` needs no context and is answered
   with one provider per effect: frame generation `3.1.6` (id
   `0xf600000000c01006`) and the swapchain wrap `3.1.7` (id `0xf65cdd1200c01007`),
   the values AMD's binary reported.
3. **Wrap the swapchain.** The wrapper takes over the game's native swapchain, as
   AMD's did, and presents the real and generated frames itself.
4. **Generate.** Each `Prepare` dispatch hands depth, motion vectors and the render
   size to Sedici's stage 1 (and later stage 2) generator; camera data in the descriptor is
   received but not used yet.
5. **Cap, when asked.** The wrapper sees exactly one `Present` per real frame.
   With a target fps set, it caps real frames at target / output ratio and targets `previous_deadline + 1/fps`, waits on a
   high-resolution waitable timer until `deadline - spin_us`, then spins on QPC to
   the deadline. A frame already past its deadline does not wait; one more than a
   full interval late reseeds the schedule instead of bursting. Capping the real
   frames puts the output at about twice the cap with 2x generation, which is the target.
6. **Observe.** Desc chains, return codes, cadence statistics and limiter
   statistics are written to `sedici.log`.

### Return codes for calls Sedici does not answer

| Call | Result |
|---|---|
| any call on a null handle or a handle Sedici never issued | `FFX_API_RETURN_ERROR_PARAMETER` |
| configure, dispatch, query or destroy on a Sedici handle that was already destroyed | `FFX_API_RETURN_ERROR`, logged |
| create whose chain names both a swapchain wrap and frame generation | `FFX_API_RETURN_ERROR`, logged |
| create with a descriptor chain that is not frame generation or the swapchain wrap | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` |
| `GetVersions` for another effect | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` |
| `GetVersions` without an output count | `FFX_API_RETURN_ERROR_PARAMETER` |
| any other query or dispatch type on a Sedici context (including the game's own `FrameGeneration` dispatch) | `FFX_API_RETURN_ERROR`, logged |
| swapchain wrap without a swapchain or a queue | `FFX_API_RETURN_ERROR`, logged |
| configure, dispatch or query on a Sedici context with a null descriptor | `FFX_API_RETURN_ERROR`, logged |
| destroy while GPU work has not retired (K127) | `FFX_API_RETURN_ERROR`, logged; the object is parked until it retires |
| frame generation create with unsupported flags, back-buffer format, depth format, motion-vector format or input geometry | `FFX_API_RETURN_ERROR`, logged |
| prepare while the previous prepare slot is still in flight (K98) | `FFX_API_RETURN_ERROR`, logged |
| `FrameGenerationSwapChainNewDX12` and `ForHwndDX12` creates | `FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE` (see [Status and limits](../../README.md#status-and-limits)) |

Every rejection is logged as `rejected: <reason> [code=E_...] [op=<call> ctx=<handle>]`. The codes
are stable and never reused: `E_STALE_HANDLE`, `E_NULL_DESCRIPTOR`, `E_INVALID_DESCRIPTOR`,
`E_UNSUPPORTED`, `E_NAMESPACE`, `E_ALLOCATION`, `E_WRAP_FAILED`, `E_NOT_RETIRED`, `E_SLOT_BUSY`,
`E_RETIREMENT_TIMEOUT`, and `E_REJECTED` for anything else. A router call that keeps the game's
thread for 50 ms or more logs `fg: slow call op=... took N ms`.

Each is logged at level 1 and above (level 0 logs internal errors only). None can crash
the host.

## DLL lifetime

A DLL that was loaded but never used unloads normally. Once a game has created a Sedici
context the DLL pins itself in memory for the rest of the process: its handle namespace,
any swapchain still waiting for the GPU, and its worker threads all live in the module, so
`FreeLibrary` becomes a no-op and a reload returns the same module. The log says
`module pinned in memory` when this happens.
