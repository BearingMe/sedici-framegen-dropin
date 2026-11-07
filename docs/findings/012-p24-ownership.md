# 012: P2.4 session, context and callback ownership

Records what P2.4 changed, how it is tested, and what it deliberately leaves for
later milestones. Nothing here has run in the game.

## What changed

| Sub-gate | Before | After |
| --- | --- | --- |
| Lookup and identity | `std::map<void*, Owned>` under one mutex that was held across `Session::configure`, `prepare` and `Swapchain::wrap`, including GPU and device calls | The registry hands out `shared_ptr` leases and its mutex guards the map only. Each owner has its own call mutex and a `dead` flag set by destroy. Creation builds the owner unlocked and registers it complete. |
| Destroy | Moved the owner out under the lock, then destroyed | Unregisters first, waits for calls already running on that owner, then tears it down; a lease taken just before destroy gets a refusal, never a torn-down owner |
| Swapchain lookup | `find_wrapper` returned a raw pointer, and `Session::configure` called `AddRef` after the lock was released | The reference is taken under the registry lock, which is safe because a wrapper is registered only while its handle holds a reference |
| Lifecycle | Implicit in `ready()`, `enabled()` and `have_history()` | `SessionState` (Empty, Ready, Warmup, Active, Draining, Stopped) with defined accepted work; configure and prepare are refused when Empty, Draining or Stopped; destroy is idempotent |
| Callback lifetime | Implicit | Documented on `GenerateSink`: the wrapper calls the sink only inside `present()` under the mutex `set_generator()` takes, so once detach returns no callback is running or will start |

## Bugs found

K27 (lock held across backend calls), K28 (use-after-free race in swapchain lookup),
K29 (call racing destroy) and K30 (implicit session state) are recorded in
[known-issues.md](../refactor/known-issues.md#found-during-p22-to-p24-refactorphase-2)
with evidence and fix commits. K28 was found by code review and was not reproduced as a
crash; the racing-destroy test covers K29, not K28's exact window.

## Tests

- `sedici_fgcheck --routing`, run in both creation orders (session first and wrap
  first): two live handles, configure, query, dispatch, a foreign handle, four
  threads calling configure and dispatch while the handle is destroyed, then a
  stale configure and a double destroy, both refused.
- `sedici_fgcheck --present` session: state is Empty before create (configure and
  prepare refused), Ready after create, Ready after FG off, Stopped after destroy
  (calls refused, destroy idempotent), and the wrapper keeps presenting with no
  further generation after detach.
- 18 of 18 CTest entries pass.

## Deferred, with owner

| Item | Why | Owner |
| --- | --- | --- |
| Backend identity and a single session object spanning an FG context and its swapchain context | Only the Sedici backend exists in the routing layer; the AMD path is still plain forwarding | P2.9 |
| Degraded, suspended and device-lost states | They need the GPU failure signals of the submission boundary | P2.5 |
| State changes logged as transitions | Diagnostics policy | P2.10 |
| Reentrancy: a callback that calls back into the wrapper | Forbidden by contract, not detected | P2.5 |
| Direct test of a failed `Session::create` | Failure is reachable only with a bad device; owners are registered only when complete, so no partial owner can be looked up | P2.5 fault injection |
