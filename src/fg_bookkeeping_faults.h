#pragma once

// Internal fault-injection seam for the offline production-object test host.
// These functions are not DLL exports and no environment switch activates
// them. Normal runtime never arms a fault.
namespace sedici::fg {

enum class BookkeepingFault {
    None,
    OwnerAllocation,
    RouteRegistryAllocation,
    WrapperRegistryAllocation,
    GenerationSignal, // offline test: simulate queue Signal failure after Execute
    GenerationClose,  // offline test: simulate a failed command-list Close
    GenerationReset,  // offline test: simulate a failed allocator Reset
    SubmissionRegistration, // offline test: reject a retirement token
    PresentParametersAllocation, // offline test: simulate dirty-rectangle allocation failure
    TokenReservation,            // offline test: simulate handle-namespace reservation failure (K29)
    BackendCreation,             // offline test: simulate backend allocation failure before creation (K63)
    PresenterSubmit,             // offline test: simulate a refused presenter job (K105)
    OverlayDraw,                 // offline test: simulate an overlay renderer failure while drawing
};

void arm_bookkeeping_fault(BookkeepingFault fault) noexcept;
bool consume_bookkeeping_fault(BookkeepingFault fault) noexcept;

// One-shot pause hook for forcing a lookup/destroy interleaving in the offline
// host. It is not exported by the proxy and is never armed in normal runtime.
using WrapperLookupHook = void (*)() noexcept;
void arm_wrapper_lookup_hook(WrapperLookupHook hook) noexcept;
void run_wrapper_lookup_hook() noexcept;

// One-shot pause after a creation publishes its owner and reads the hooks, so a
// concurrent hook change can be forced into the window (K60).
using CreationHook = void (*)() noexcept;
void arm_creation_hook(CreationHook hook) noexcept;
void run_creation_hook() noexcept;

using ContextRecordHook = void (*)() noexcept;
void arm_context_record_hook(ContextRecordHook hook) noexcept;
void run_context_record_hook() noexcept;

using QueueRecoveryHook = void (*)() noexcept;
void arm_queue_recovery_hook(QueueRecoveryHook hook) noexcept;
void run_queue_recovery_hook() noexcept;
void note_swapchain_retention() noexcept;
unsigned swapchain_retention_count() noexcept;

} // namespace sedici::fg
