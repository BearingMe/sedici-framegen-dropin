#pragma once

#include "settings/settings_tx.h"

#include <cstdint>

namespace sedici {

enum class SettingsIoKind { BeginEdit, Save };

struct SettingsIoCompletion {
    SettingsIoKind kind = SettingsIoKind::BeginEdit;
    uint64_t viewEpoch = 0;
    uint32_t workerThreadId = 0;
    bool beginOk = false;
    SaveResult save;
    // The worker did not finish within the stall limit: the request is reported as
    // unconfirmed so the menu is not left waiting. `save.message` says so (K83).
    bool stalled = false;
};

// At most one job and one unconsumed completion exist. Start functions perform
// no file I/O; a worker owns a module reference until it exits. A false return
// means no job was started. The UI must poll before starting another job.
bool settings_io_start_begin(uint64_t editGeneration, uint64_t viewEpoch) noexcept;
bool settings_io_start_save(SettingsSaveTicket ticket, uint64_t viewEpoch) noexcept;
bool settings_io_busy() noexcept;
bool settings_io_poll(SettingsIoCompletion& result) noexcept;

// K83: a worker blocked in the filesystem cannot be cancelled safely, but it must not
// leave the menu waiting forever. After kSettingsIoStallMs the poll reports the job as
// stalled (once), tries to cancel its synchronous I/O, and discards the late result.
// The worker count stays one: no new job starts until the stalled worker exits.
inline constexpr unsigned kSettingsIoStallMs = 5000;
uint64_t settings_io_stall_count() noexcept;   // jobs reported stalled so far
bool settings_io_stalled() noexcept;           // a reported-stalled worker is still running

// Offline test seams; never called by the DLL. The block event stands in for a hung
// filesystem: the worker waits on it (bounded to 30 s) before doing its I/O.
void settings_io_test_set_stall_ms(unsigned ms) noexcept;
void settings_io_test_set_block(void* event_handle) noexcept;

} // namespace sedici
