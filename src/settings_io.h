#pragma once

#include "settings_tx.h"

#include <cstdint>

namespace sedici {

enum class SettingsIoKind { BeginEdit, Save };

struct SettingsIoCompletion {
    SettingsIoKind kind = SettingsIoKind::BeginEdit;
    uint64_t viewEpoch = 0;
    uint32_t workerThreadId = 0;
    bool beginOk = false;
    SaveResult save;
};

// At most one job and one unconsumed completion exist. Start functions perform
// no file I/O; a worker owns a module reference until it exits. A false return
// means no job was started. The UI must poll before starting another job.
bool settings_io_start_begin(uint64_t editGeneration, uint64_t viewEpoch) noexcept;
bool settings_io_start_save(SettingsSaveTicket ticket, uint64_t viewEpoch) noexcept;
bool settings_io_busy() noexcept;
bool settings_io_poll(SettingsIoCompletion& result) noexcept;

} // namespace sedici
