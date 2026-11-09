#include "runtime/bookkeeping_faults.h"

#include <atomic>

namespace sedici::fg {
namespace {
std::atomic<BookkeepingFault> g_armed{BookkeepingFault::None};
std::atomic<WrapperLookupHook> g_lookup_hook{nullptr};
std::atomic<ContextRecordHook> g_record_hook{nullptr};
std::atomic<QueueRecoveryHook> g_recovery_hook{nullptr};
std::atomic<CreationHook> g_creation_hook{nullptr};
std::atomic<unsigned> g_swapchain_retentions{0};
std::atomic<unsigned> g_swapchain_releases{0};
std::atomic<bool> g_retirement_hold{false};
}

void set_retirement_hold(bool held) noexcept { g_retirement_hold.store(held, std::memory_order_release); }
bool retirement_held() noexcept { return g_retirement_hold.load(std::memory_order_acquire); }

void note_swapchain_release() noexcept
{
    g_swapchain_releases.fetch_add(1, std::memory_order_relaxed);
}

unsigned swapchain_release_count() noexcept
{
    return g_swapchain_releases.load(std::memory_order_relaxed);
}

void arm_bookkeeping_fault(BookkeepingFault fault) noexcept
{
    g_armed.store(fault, std::memory_order_release);
}

bool consume_bookkeeping_fault(BookkeepingFault fault) noexcept
{
    BookkeepingFault expected = fault;
    return g_armed.compare_exchange_strong(expected, BookkeepingFault::None,
                                           std::memory_order_acq_rel);
}

void arm_wrapper_lookup_hook(WrapperLookupHook hook) noexcept
{
    g_lookup_hook.store(hook, std::memory_order_release);
}

void run_wrapper_lookup_hook() noexcept
{
    const WrapperLookupHook hook = g_lookup_hook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook) hook();
}

void arm_creation_hook(CreationHook hook) noexcept
{
    g_creation_hook.store(hook, std::memory_order_release);
}

void run_creation_hook() noexcept
{
    const CreationHook hook = g_creation_hook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook) hook();
}

void arm_context_record_hook(ContextRecordHook hook) noexcept
{
    g_record_hook.store(hook, std::memory_order_release);
}

void run_context_record_hook() noexcept
{
    const ContextRecordHook hook = g_record_hook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook) hook();
}

void arm_queue_recovery_hook(QueueRecoveryHook hook) noexcept
{
    g_recovery_hook.store(hook, std::memory_order_release);
}

void run_queue_recovery_hook() noexcept
{
    const QueueRecoveryHook hook = g_recovery_hook.exchange(nullptr, std::memory_order_acq_rel);
    if (hook) hook();
}

void note_swapchain_retention() noexcept
{
    g_swapchain_retentions.fetch_add(1, std::memory_order_relaxed);
}

unsigned swapchain_retention_count() noexcept
{
    return g_swapchain_retentions.load(std::memory_order_relaxed);
}

} // namespace sedici::fg
