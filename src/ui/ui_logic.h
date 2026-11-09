#pragma once

// Pure logic behind the Full menu's controls (P4.2). No ImGui, no state, so the
// regression host tests it without a window.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sedici {

// FR-04: the menu scale choices (besides Auto).
inline constexpr float kScaleChoices[] = {0.8f, 0.9f, 1.0f};
inline constexpr std::size_t kScaleChoiceCount = sizeof(kScaleChoices) / sizeof(kScaleChoices[0]);

// Index into kScaleChoices of the choice closest to `value`. A value from an
// older file (0.5, 1.75, ...) snaps to the nearest choice.
inline std::size_t nearest_scale_choice(float value) noexcept
{
    std::size_t best = 0;
    float bestDistance = 1e9f;
    for (std::size_t i = 0; i < kScaleChoiceCount; ++i) {
        const float distance = std::fabs(value - kScaleChoices[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

// FR-01: the target slider works on whole steps of half a frame per second, so
// the handle can only land on a legal value and nothing is re-read from the live
// value while it is dragged (K118).
inline constexpr int kTargetMinUnits = 40;  // 20 fps
inline constexpr int kTargetMaxUnits = 480; // 240 fps

inline int target_units_from_fps(double fps) noexcept
{
    if (!std::isfinite(fps)) return kTargetMinUnits;
    const long units = std::lround(fps * 2.0);
    return static_cast<int>(std::clamp<long>(units, kTargetMinUnits, kTargetMaxUnits));
}

inline double target_fps_from_units(int units) noexcept
{
    return std::clamp(units, kTargetMinUnits, kTargetMaxUnits) / 2.0;
}

// The Full menu edits a pending value and applies it with one button (FR-02).
// `pending` follows the live value until the user touches the slider.
struct PendingTarget {
    int units = 73; // 36.5 fps
    bool dirty = false;
};

inline void pending_follow_live(PendingTarget& pending, double liveFps) noexcept
{
    if (!pending.dirty && liveFps > 0.0) pending.units = target_units_from_fps(liveFps);
}

inline void pending_edit(PendingTarget& pending, int units) noexcept
{
    pending.units = std::clamp(units, kTargetMinUnits, kTargetMaxUnits);
    pending.dirty = true;
}

// Returns the value to apply and clears the pending state.
inline double pending_apply(PendingTarget& pending) noexcept
{
    pending.dirty = false;
    return target_fps_from_units(pending.units);
}

// K126: Alt+F4 must reach the game in every menu mode. The Full menu consumes
// keyboard messages for itself, except the system close key. `lparam` bit 29 is
// set while Alt is held.
inline bool ui_passes_to_game(unsigned msg, unsigned long long wparam, long long lparam) noexcept
{
    constexpr unsigned kSysKeyDown = 0x0104, kSysKeyUp = 0x0105; // WM_SYSKEYDOWN, WM_SYSKEYUP
    constexpr unsigned long long kVkF4 = 0x73;                  // VK_F4
    return (msg == kSysKeyDown || msg == kSysKeyUp) && wparam == kVkF4 &&
           ((static_cast<unsigned long long>(lparam) >> 29) & 1ull) != 0;
}

} // namespace sedici
