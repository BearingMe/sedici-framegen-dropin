#pragma once

#include "ffx_api.h"

// Resolved entry points of amd_fidelityfx_framegeneration_dx12.dll.orig.
extern PfnFfxCreateContext g_ffxCreateContext;
extern PfnFfxDestroyContext g_ffxDestroyContext;
extern PfnFfxConfigure g_ffxConfigure;
extern PfnFfxQuery g_ffxQuery;
extern PfnFfxDispatch g_ffxDispatch;

// Loads the original DLL lazily. Returns true only when all five exports resolved.
bool ensure_original_loaded() noexcept;
