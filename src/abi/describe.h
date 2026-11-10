#pragma once

#include "ffx_api.h"

enum class FfxExportKind {
    Create = 0,
    Destroy = 1,
    Configure = 2,
    Query = 3,
    Dispatch = 4,
};

const char* ffx_export_name(FfxExportKind kind) noexcept;

// Human readable name for an FFX return code.
const char* ffx_return_code_name(ffxReturnCode_t code) noexcept;

// Logs the call when the level/novelty policy allows it. `context` is the
// ffxContext value, not a pointer to the ffxContext slot.
void describe_call(FfxExportKind kind, const ffxApiHeader* desc, ffxReturnCode_t result, void* context) noexcept;
