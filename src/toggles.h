#pragma once

#include "ffx_api.h"

// Opt-in experiment toggles. Both are inert unless their environment variable
// is set, so an unset environment stays bit-identical to milestone 0.
//
//   SEDICI_ASYNC=0        Clears FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT
//                         from the frame generation create flags and forces
//                         allowAsyncWorkloads=false in every frame generation
//                         configure desc.
//   SEDICI_PACING=hybrid  After the frame generation swapchain context is
//                         created or wrapped, sends the swapchain frame pacing
//                         tuning configure with allowHybridSpin=true.
bool toggles_async_disable() noexcept;
bool toggles_pacing_hybrid() noexcept;

// Rewrites the caller's desc chain in place for the duration of one call, then
// restores every byte it touched. Nested descs are handled; the walk is bounded
// and cycle safe. Does nothing when no toggle applies.
class ScopedDescOverride {
public:
    explicit ScopedDescOverride(const ffxApiHeader* desc) noexcept;
    ~ScopedDescOverride();

    ScopedDescOverride(const ScopedDescOverride&) = delete;
    ScopedDescOverride& operator=(const ScopedDescOverride&) = delete;

private:
    struct Restore {
        void* address;
        size_t size;
        uint64_t saved;
    };

    void patch(void* address, size_t size, uint64_t value) noexcept;

    Restore m_restore[8]{};
    size_t m_count = 0; // 0 means "nothing overridden".
};

// Called after a successful ffxCreateContext. Sends the pacing tuning desc when
// the create chain contains a frame generation swapchain desc.
void toggles_after_create(ffxContext* context, const ffxCreateContextDescHeader* desc) noexcept;

// Called after ffxConfigure. Retries the pacing tuning desc on the frame
// generation context when the swapchain context rejected it.
void toggles_after_configure(ffxContext* context, const ffxConfigureDescHeader* desc,
                             ffxReturnCode_t result) noexcept;
