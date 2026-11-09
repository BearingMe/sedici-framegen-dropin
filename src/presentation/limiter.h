#pragma once

#include <cstdint>

// Real-frame limiter.
//
// Called from the swapchain Present/Present1 hooks immediately before the
// original Present, with the QPC timestamp taken on entry. The hook is
// installed on the frame-generation swapchain wrapper, which sees exactly one
// Present per real frame; the generated frames are presented inside the
// wrapper and never reach this code. Capping here therefore caps the game's
// real frames, and with 2x frame generation the output lands at about twice
// the configured rate.
//
// Inert unless `[limiter] fps` (or SEDICI_LIMIT_FPS) is greater than zero. When
// inert it does no work at all: no timer, no wait, no allocation, no log.
void limiter_before_present(uint64_t now_qpc) noexcept;

// Reports whether frame generation is active, for the limiter's log label. Set
// once by the DLL entry; unset (tests) means "off".
void limiter_set_fg_probe(bool (*probe)() noexcept) noexcept;

// Rebuilds the limiter schedule after the live target changes. Safe to call
// from the overlay thread; no timer is created here, the next real Present
// initializes with the new target.
void limiter_on_config_changed() noexcept;
