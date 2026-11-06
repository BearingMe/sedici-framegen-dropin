#pragma once

#include "ffx_api.h"

// The two behaviour switches, both resolved from sedici.toml with environment
// overrides. See config.h and README.md.
//
//   pacing.mode = "hybrid"   After the frame generation swapchain context is
//                            created, sends the swapchain frame pacing tuning
//                            configure with allowHybridSpin=true.
//   async.mode  = "off"      Forces allowAsyncWorkloads=false in every frame
//                            generation configure desc. The create flags are
//                            left as the game set them.
//
// Both default to the milestone-1 behaviour (pacing hybrid, async untouched).
// Setting pacing.mode="off" and async.mode="game" makes Sedici forward exactly
// what it did in milestone 0.
bool toggles_async_disable() noexcept;
bool toggles_pacing_hybrid() noexcept;

// Re-sends the pacing tuning to every tracked swapchain context after the
// overlay changes the live mode. `hybrid` selects the configured hybrid values;
// otherwise the SDK defaults are sent. Safe to call from the overlay thread.
void toggles_apply_pacing_live() noexcept;

// The return code of the most recent pacing tuning send, or INT_MIN before one.
int toggles_last_pacing_result() noexcept;

// True while a frame generation context exists and the last frame generation
// configure told the runtime that generation is enabled. Used only to label the
// limiter's mode ("fg=on/off"); the limiter caps real frames either way.
bool toggles_fg_active() noexcept;

// Rewrites the caller's desc chain in place for the duration of one call, then
// restores every byte it touched. Nested descs are handled; the walk is bounded
// and cycle safe. Does nothing when async.mode is not "off".
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

// Called after a successful ffxCreateContext. Records the swapchain for the
// stall detector, and sends the pacing tuning desc when this call created or
// wrapped a frame generation swapchain.
//
// State is tracked per frame generation context, so a swapchain recreated by
// the game (resolution change, frame generation toggled) gets its own send
// instead of inheriting the previous context's "already applied" flag.
void toggles_after_create(ffxContext* context, const ffxCreateContextDescHeader* desc) noexcept;

// Called after ffxConfigure. Records the event for the stall detector and
// retries the pacing tuning desc on the frame generation context if the
// swapchain path did not accept it.
void toggles_after_configure(ffxContext* context, const ffxConfigureDescHeader* desc,
                             ffxReturnCode_t result) noexcept;

// Called before the original ffxDestroyContext, to drop the per-context pacing
// state. Without this a recycled context pointer would look "already applied".
// Takes the context value (the `void*` the game holds), not the slot pointer.
void toggles_on_destroy(void* context) noexcept;
