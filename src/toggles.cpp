#include "toggles.h"

#include "config.h"
#include "describe.h"
#include "forward.h"
#include "log.h"
#include "stats.h"

#include "ffx_framegeneration.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4996) // The SDK marks the legacy prepare desc deprecated.

namespace {

constexpr size_t kMaxChainItems = 64;

// How many ffxConfigure attempts a single context gets before Sedici gives up.
// In practice the first one succeeds; the bound exists so a rejecting driver
// cannot turn every configure into an extra call for the rest of the session.
constexpr int kMaxPacingAttempts = 5;

// Contexts are created and destroyed rarely. The cap only exists so a game that
// leaks contexts cannot grow this without bound.
constexpr size_t kMaxTrackedContexts = 32;

std::string hx(const void* value)
{
    std::ostringstream out;
    out << "0x" << std::hex << reinterpret_cast<uintptr_t>(value);
    return out.str();
}

void log_async_once() noexcept
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        log_event(1, "sedici: async.mode=off active - cleared AsyncWorkloads (0x1) from the frame "
                     "generation create flags and forcing allowAsyncWorkloads=false in frame "
                     "generation configure descs");
    });
}

void log_pacing_once() noexcept
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        log_event(1, "sedici: pacing.mode=hybrid active - sending "
                     "FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 "
                     "(0x30008) key=2 (FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING) "
                     "once per frame generation swapchain context");
    });
}

// Whether the chain contains a frame generation swapchain create desc, and the
// swapchain it names. `pointer` is null when the out-pointer had not been
// filled in, which is not a reason to skip the send: whether the *desc* is
// present is what decides that.
struct SwapchainRef {
    bool present = false;
    const void* pointer = nullptr;
};

SwapchainRef find_swapchain(const ffxApiHeader* desc) noexcept
{
    SwapchainRef ref;
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        switch (item->type) {
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12: {
            const auto& d =
                reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainWrapDX12&>(*item);
            ref.present = true;
            if (d.swapchain && *d.swapchain) ref.pointer = static_cast<const void*>(*d.swapchain);
            return ref;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12: {
            const auto& d =
                reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainNewDX12&>(*item);
            ref.present = true;
            if (d.swapchain && *d.swapchain) ref.pointer = static_cast<const void*>(*d.swapchain);
            return ref;
        }
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12: {
            const auto& d = reinterpret_cast<
                const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12&>(*item);
            ref.present = true;
            if (d.swapchain && *d.swapchain) ref.pointer = static_cast<const void*>(*d.swapchain);
            return ref;
        }
        default:
            break;
        }
        if (seen.size() >= kMaxChainItems) break;
    }
    return ref;
}

bool chain_has_fg_configure_with_swapchain(const ffxApiHeader* desc) noexcept
{
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        switch (item->type) {
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION:
            if (reinterpret_cast<const ffxConfigureDescFrameGeneration&>(*item).swapChain) return true;
            break;
        case FFX_API_FRAME_GENERATION_CONFIG:
            if (reinterpret_cast<const FfxFrameGenerationConfig&>(*item).swapChain) return true;
            break;
        default:
            break;
        }
        if (seen.size() >= kMaxChainItems) break;
    }
    return false;
}

// Per frame generation context pacing state.
struct ContextPacing {
    bool tracked = false;      // see `swapchain`
    const void* swapchain = nullptr;
    bool applied = false;      // the tuning was accepted
    int attempts = 0;          // ffxConfigure calls made for this context
};

std::mutex g_pacing_mutex;
std::map<void*, ContextPacing> g_pacing_contexts;

// Caller holds g_pacing_mutex. Returns nullptr only when the table is full and
// the context is new, which means "do not track, do not send".
ContextPacing* acquire_state(void* key) noexcept
{
    const auto existing = g_pacing_contexts.find(key);
    if (existing != g_pacing_contexts.end()) return &existing->second;
    if (g_pacing_contexts.size() >= kMaxTrackedContexts) return nullptr;
    return &g_pacing_contexts.emplace(key, ContextPacing{}).first->second;
}

// Sends the tuning desc and logs it. Returns the original DLL's return code so
// the caller can update its per-context state. Deliberately called without
// g_pacing_mutex held: it enters the original DLL, and holding our own lock
// across foreign code is a needless deadlock risk.
ffxReturnCode_t send_pacing_tuning(ffxContext* context, const char* reason) noexcept
{
    if (!context || !*context || !g_ffxConfigure) return FFX_API_RETURN_ERROR;

    const sedici::PacingSettings& pacing = sedici::settings().pacing;

    FfxApiSwapchainFramePacingTuning tuning{};
    tuning.safetyMarginInMs = pacing.safety_margin_ms;
    tuning.varianceFactor = pacing.variance_factor;
    tuning.allowHybridSpin = true;
    tuning.hybridSpinTime = pacing.hybrid_spin_time;
    tuning.allowWaitForSingleObjectOnFence = pacing.wait_for_single_object_on_fence;

    ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
    config.header.pNext = nullptr;
    config.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
    config.u64 = 0;
    config.ptr = &tuning;

    const ffxReturnCode_t result = g_ffxConfigure(context, &config.header);

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "sedici: FramePacingTuning sent after " << reason
        << " ctx=" << hx(*context)
        << " key=" << config.key
        << " safetyMarginInMs=" << tuning.safetyMarginInMs
        << " varianceFactor=" << tuning.varianceFactor
        << " allowHybridSpin=" << (tuning.allowHybridSpin ? "true" : "false")
        << " hybridSpinTime=" << tuning.hybridSpinTime
        << " allowWaitForSingleObjectOnFence=" << (tuning.allowWaitForSingleObjectOnFence ? "true" : "false")
        << " result=" << static_cast<int>(result) << " (" << ffx_return_code_name(result) << ')';
    log_event(1, out.str());

    if (result != FFX_API_RETURN_OK) {
        // Not fatal: the call has already been forwarded, and the game must not
        // see a different return code because of an optional tuning tweak.
        std::ostringstream note;
        note << "sedici: frame pacing tuning was rejected (result=" << static_cast<int>(result)
             << "); forwarding continues unchanged and the retry path through ffxConfigure "
                "stays armed";
        log_event(1, note.str());
    }

    return result;
}

void record_attempt(void* key, ffxReturnCode_t result) noexcept
{
    std::lock_guard<std::mutex> lock(g_pacing_mutex);
    ContextPacing* state = acquire_state(key);
    if (!state) return;
    state->attempts += 1;
    if (result == FFX_API_RETURN_OK) state->applied = true;
}

} // namespace

bool toggles_async_disable() noexcept
{
    return sedici::settings().async.mode == sedici::AsyncMode::Off;
}

bool toggles_pacing_hybrid() noexcept
{
    return sedici::settings().pacing.mode == sedici::PacingMode::Hybrid;
}

void ScopedDescOverride::patch(void* address, size_t size, uint64_t value) noexcept
{
    if (!address || size == 0 || size > sizeof(uint64_t) || m_count >= std::size(m_restore)) return;

    uint64_t saved = 0;
    std::memcpy(&saved, address, size);
    if (saved == value) return;

    std::memcpy(address, &value, size);
    m_restore[m_count++] = Restore{address, size, saved};
}

ScopedDescOverride::ScopedDescOverride(const ffxApiHeader* desc) noexcept
{
    if (!desc || !toggles_async_disable()) return;

    try {
        log_async_once();

        std::set<const ffxApiHeader*> seen;
        for (const ffxApiHeader* item = desc; item; item = item->pNext) {
            if (!seen.insert(item).second) break;

            switch (item->type) {
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION: {
                auto* d = const_cast<ffxCreateContextDescFrameGeneration*>(
                    reinterpret_cast<const ffxCreateContextDescFrameGeneration*>(item));
                patch(&d->flags, sizeof(d->flags),
                      d->flags & ~static_cast<uint32_t>(FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT));
                break;
            }
            case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
                auto* d = const_cast<ffxConfigureDescFrameGeneration*>(
                    reinterpret_cast<const ffxConfigureDescFrameGeneration*>(item));
                patch(&d->allowAsyncWorkloads, sizeof(d->allowAsyncWorkloads), 0);
                break;
            }
            case FFX_API_FRAME_GENERATION_CONFIG: {
                auto* d = const_cast<FfxFrameGenerationConfig*>(
                    reinterpret_cast<const FfxFrameGenerationConfig*>(item));
                patch(&d->allowAsyncWorkloads, sizeof(d->allowAsyncWorkloads), 0);
                break;
            }
            default:
                break;
            }

            if (seen.size() >= kMaxChainItems) break;
        }
    } catch (...) {
        log_error("failed to apply the async.mode=off override");
    }
}

ScopedDescOverride::~ScopedDescOverride()
{
    for (size_t i = m_count; i > 0; --i) {
        const Restore& entry = m_restore[i - 1];
        uint64_t saved = entry.saved;
        std::memcpy(entry.address, &saved, entry.size);
    }
    m_count = 0;
}

void toggles_after_create(ffxContext* context, const ffxCreateContextDescHeader* desc) noexcept
{
    if (!desc) return;

    try {
        // Recorded even when pacing is off, so the stall detector still knows
        // that the swapchain was rebuilt.
        const SwapchainRef swapchain = find_swapchain(desc);
        if (!swapchain.present) return;
        stats_on_swapchain_event("swapchain-create");

        void* key = context ? *context : nullptr;
        if (!key) return;

        bool send = false;
        {
            std::lock_guard<std::mutex> lock(g_pacing_mutex);
            ContextPacing* state = acquire_state(key);
            if (!state) return;

            // Only a swapchain pointer we could actually read can prove that the
            // swapchain changed. When it is null, keep whatever we had.
            if (state->tracked && swapchain.pointer && state->swapchain &&
                state->swapchain != swapchain.pointer) {
                // Same context, new swapchain: the game recreated it (resolution
                // change, frame generation toggled). The previous tuning does
                // not apply to the new one.
                state->applied = false;
                state->attempts = 0;
                log_event(1, "sedici: frame generation swapchain recreated on the same context; "
                             "pacing tuning will be sent again");
            }

            if (swapchain.pointer) state->swapchain = swapchain.pointer;
            state->tracked = true;

            if (!toggles_pacing_hybrid()) return;
            if (state->applied) return;
            if (state->attempts >= kMaxPacingAttempts) return;
            send = true;
        }

        if (!send) return;
        log_pacing_once();
        record_attempt(key, send_pacing_tuning(context, "swapchain create"));
    } catch (...) {
        log_error("failed to send the frame pacing tuning after swapchain creation");
    }
}

void toggles_after_configure(ffxContext* context, const ffxConfigureDescHeader* desc,
                             ffxReturnCode_t result) noexcept
{
    if (!desc) return;

    try {
        // The stall detector cares about any configure reaching the DLL, not
        // just the ones Sedici takes an interest in.
        if (result == FFX_API_RETURN_OK) stats_on_swapchain_event("configure");

        if (!toggles_pacing_hybrid()) return;
        if (result != FFX_API_RETURN_OK) return;
        if (!chain_has_fg_configure_with_swapchain(desc)) return;

        void* key = context ? *context : nullptr;
        if (!key) return;

        {
            std::lock_guard<std::mutex> lock(g_pacing_mutex);
            const auto state = g_pacing_contexts.find(key);
            if (state != g_pacing_contexts.end()) {
                if (state->second.applied) return;
                if (state->second.attempts >= kMaxPacingAttempts) return;
            } else if (g_pacing_contexts.size() >= kMaxTrackedContexts) {
                return;
            }
        }

        record_attempt(key, send_pacing_tuning(context, "frame generation configure"));
    } catch (...) {
        log_error("failed to send the frame pacing tuning after configure");
    }
}

void toggles_on_destroy(void* context) noexcept
{
    try {
        if (!context) return;
        std::lock_guard<std::mutex> lock(g_pacing_mutex);
        g_pacing_contexts.erase(context);
    } catch (...) {
        log_error("failed to drop the frame pacing state for a destroyed context");
    }
}

#pragma warning(pop)
