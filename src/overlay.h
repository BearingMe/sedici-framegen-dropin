#pragma once

#include "ffx_framegeneration.h"

#include <cstddef>
#include <cstdint>

// Sedici's frame-generation present callback.
//
// FFX calls the callback once per presented output frame, after frame
// generation: once for each real frame and once for each generated frame. That
// makes it the only place that sees the final output rate, and the only place
// that can draw on the final image without the UI being interpolated.
//
// A non-null presentCallback *replaces* FFX's default composition callback.
// Sedici therefore only takes over in the configuration Cyberpunk uses: no
// separate UI resource is registered, so the required operation is exactly the
// back-buffer copy AMD's default performs. If a UI resource is registered, or
// the copy cannot be performed, Sedici leaves the callback null and lets FFX
// keep its own composition path.

// Scoped: rewrites the frame generation configure desc in the caller's chain so
// its `presentCallback` is Sedici's trampoline for the duration of the call,
// then restores every byte it touched. Only the two callback fields are
// touched; HUDLessColor, flags, frameID and everything else are left alone.
//
// If the game set its own callback, it is remembered and chained: Sedici's
// trampoline runs first, then calls the game's callback with the game's own
// user context and returns its return code.
class ScopedOverlayCallback {
public:
    explicit ScopedOverlayCallback(const ffxApiHeader* desc) noexcept;
    ~ScopedOverlayCallback();

    ScopedOverlayCallback(const ScopedOverlayCallback&) = delete;
    ScopedOverlayCallback& operator=(const ScopedOverlayCallback&) = delete;

private:
    struct Restore {
        void* address;
        size_t size;
        uint64_t saved;
    };

    void patch(void* address, size_t size, uint64_t value) noexcept;

    Restore m_restore[8]{};
    size_t m_count = 0;
};

// Called from the trampoline on every presented output frame. Records the
// timestamp and the real/generated split, and logs output fps every 300 calls.
// Never lets an exception escape into FFX.
void overlay_on_present(const ffxCallbackDescFrameGenerationPresent* params) noexcept;
