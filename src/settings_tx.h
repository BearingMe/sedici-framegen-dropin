#pragma once

#include "live.h"

#include <cstdint>
#include <filesystem>
#include <string>

// Transactional access to the live settings (P2.6).
//
// Every change to a live value goes through one complete candidate: it is
// validated as a whole, published as a coherent snapshot, and given a new revision.
// Nothing writes the individual live values
// independently any more, and no caller resets a subsystem by hand: the apply
// notifies the limiter and pacing code itself, once, when their value changed.
//
// A request names the revision it was built from. If another change landed first
// the request is a Conflict and nothing is applied, so two writers can never
// silently overwrite each other. `[ui] mode` is menu state, not a setting.
namespace sedici {

struct SettingsSnapshot {
    uint64_t revision = 0;
    LiveValues values;
};

enum class ApplyStatus {
    Applied,   // immediately published fields changed; `revision` advanced
    Pending,   // published, but pacing and/or async need later subsystem work
    Unchanged, // the candidate equals the current values; no revision, no side effects
    Rejected,  // a value is out of range or invalid; nothing was applied
    Conflict,  // built from a stale revision; nothing was applied
};

struct ApplyResult {
    ApplyStatus status = ApplyStatus::Rejected;
    uint64_t revision = 0; // current revision after the call
    std::string reason;    // set for Rejected and Conflict
};

// Published desired values, not a guarantee that deferred subsystems have applied them.
SettingsSnapshot settings_snapshot() noexcept;

// Validates `candidate`, then applies it only if `baseRevision` is still current.
ApplyResult settings_apply(uint64_t baseRevision, const LiveValues& candidate) noexcept;

// Convenience for a single-control change: reads the snapshot, lets `mutate` edit a
// copy, and applies it against that snapshot's revision. A concurrent change makes
// it a Conflict; the caller keeps showing the acknowledged values.
template <typename Mutate>
ApplyResult settings_change(Mutate&& mutate) noexcept
{
    const SettingsSnapshot base = settings_snapshot();
    LiveValues next = base.values;
    mutate(next);
    return settings_apply(base.revision, next);
}

// Edit session for the Full menu: remembers the acknowledged values and the
// configuration file's state, so cancel returns to them and save can tell that the
// file was edited externally.
bool settings_begin_edit() noexcept;
// Split edit initialization lets UI reserve an epoch without disk I/O, then
// inspect the file on an I/O worker. Invalidated epochs cannot publish a baseline.
uint64_t settings_start_edit() noexcept;
void settings_invalidate_edit() noexcept;
bool settings_begin_edit_for(uint64_t generation) noexcept;
// Applies a menu edit only if no outside writer changed the edit session.
ApplyResult settings_apply_edit(uint64_t baseRevision, const LiveValues& candidate) noexcept;
template <typename Mutate>
ApplyResult settings_change_edit(Mutate&& mutate) noexcept
{
    const SettingsSnapshot base = settings_snapshot();
    LiveValues next = base.values;
    mutate(next);
    return settings_apply_edit(base.revision, next);
}
ApplyResult settings_cancel_edit() noexcept;

struct SaveResult {
    bool ok = false;
    bool conflict = false; // the file changed on disk since the edit began; not written
    std::string message;   // "Saved" or the reason it was not
};

struct SettingsSaveTicket {
    uint64_t generation = 0;
    uint64_t revision = 0;
    bool valid = false;
};

// Capture only in-memory state; a worker must revalidate the ticket before I/O.
SettingsSaveTicket settings_capture_save_ticket() noexcept;
// Writes the published snapshot to `sedici.toml`, preserving comments. Values set
// by an environment override are not written. External-writer atomicity remains
// unresolved (K43). Never throws; failure is a result.
SaveResult settings_save() noexcept;
SaveResult settings_save_for(SettingsSaveTicket ticket) noexcept;

// The file `settings_save` writes. Defaults to `sedici.toml` beside the DLL;
// tests point it at a fixture.
void settings_use_file(const std::filesystem::path& path) noexcept;

// One-shot internal synchronization seam for offline persistence tests; not a
// DLL export and never armed by normal runtime.
using SettingsIoTestHook = void (*)() noexcept;
void settings_arm_io_test_hook(SettingsIoTestHook hook) noexcept;
void settings_arm_after_write_test_hook(SettingsIoTestHook hook) noexcept;
void settings_fail_after_write_once() noexcept;

} // namespace sedici
