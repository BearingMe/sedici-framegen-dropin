#include "toggles.h"

#include "describe.h"
#include "forward.h"
#include "log.h"

#include "ffx_framegeneration.h"
#include "ffx_api_framegeneration_dx12.h"

#include <windows.h>

#include <cstring>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

#pragma warning(push)
#pragma warning(disable : 4996) // The SDK marks the legacy prepare desc deprecated.

namespace {

constexpr size_t kMaxChainItems = 64;
constexpr int kMaxPacingAttempts = 5;

bool env_equals(const char* name, const char* expected) noexcept
{
    char value[32]{};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return false;

    for (size_t i = 0;; ++i) {
        char actual = value[i];
        const char wanted = expected[i];
        if (actual == '\0' || wanted == '\0') return actual == wanted;
        if (actual >= 'A' && actual <= 'Z') actual = static_cast<char>(actual + ('a' - 'A'));
        if (actual != wanted) return false;
    }
}

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
        log_event(1, "sedici: SEDICI_ASYNC=0 active - cleared AsyncWorkloads (0x1) from the frame "
                     "generation create flags and forcing allowAsyncWorkloads=false in frame "
                     "generation configure descs");
    });
}

void log_pacing_once() noexcept
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        log_event(1, "sedici: SEDICI_PACING=hybrid active - sending "
                     "FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 "
                     "(0x30008) key=2 (FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING) "
                     "after the frame generation swapchain is created");
    });
}

bool chain_has_swapchain_create(const ffxApiHeader* desc) noexcept
{
    std::set<const ffxApiHeader*> seen;
    for (const ffxApiHeader* item = desc; item; item = item->pNext) {
        if (!seen.insert(item).second) break;
        switch (item->type) {
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12:
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12:
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12:
            return true;
        default:
            break;
        }
        if (seen.size() >= kMaxChainItems) break;
    }
    return false;
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

struct PacingState {
    std::mutex mutex;
    bool applied = false;
    int attempts = 0;
};

PacingState& pacing_state() noexcept
{
    static PacingState state;
    return state;
}

void send_pacing_tuning(ffxContext* context, const char* reason) noexcept
{
    if (!context || !*context || !g_ffxConfigure) return;

    // Documented defaults (ffx_framegeneration_api_types.h), plus hybrid spin.
    FfxApiSwapchainFramePacingTuning tuning{};
    tuning.safetyMarginInMs = 0.1f;
    tuning.varianceFactor = 0.1f;
    tuning.allowHybridSpin = true;
    tuning.hybridSpinTime = 2;
    tuning.allowWaitForSingleObjectOnFence = false;

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

    std::lock_guard<std::mutex> lock(pacing_state().mutex);
    pacing_state().attempts += 1;
    if (result == FFX_API_RETURN_OK) pacing_state().applied = true;
}

} // namespace

bool toggles_async_disable() noexcept
{
    static const bool value = env_equals("SEDICI_ASYNC", "0");
    return value;
}

bool toggles_pacing_hybrid() noexcept
{
    static const bool value = env_equals("SEDICI_PACING", "hybrid");
    return value;
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
        log_error("failed to apply the SEDICI_ASYNC override");
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
    if (!desc || !toggles_pacing_hybrid()) return;

    try {
        log_pacing_once();
        if (!chain_has_swapchain_create(desc)) return;

        {
            std::lock_guard<std::mutex> lock(pacing_state().mutex);
            if (pacing_state().applied) return;
            if (pacing_state().attempts >= kMaxPacingAttempts) return;
        }
        send_pacing_tuning(context, "swapchain create");
    } catch (...) {
        log_error("failed to send the frame pacing tuning after swapchain creation");
    }
}

void toggles_after_configure(ffxContext* context, const ffxConfigureDescHeader* desc,
                             ffxReturnCode_t result) noexcept
{
    if (!desc || !toggles_pacing_hybrid() || result != FFX_API_RETURN_OK) return;

    try {
        {
            std::lock_guard<std::mutex> lock(pacing_state().mutex);
            if (pacing_state().applied) return;
            if (pacing_state().attempts >= kMaxPacingAttempts) return;
        }
        if (!chain_has_fg_configure_with_swapchain(desc)) return;
        send_pacing_tuning(context, "frame generation configure");
    } catch (...) {
        log_error("failed to send the frame pacing tuning after configure");
    }
}

#pragma warning(pop)
