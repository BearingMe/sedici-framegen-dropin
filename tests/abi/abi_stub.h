#pragma once

// Shared between the stub "original" frame-generation DLL (tests/abi_stub.cpp)
// and the export-level harness (tests/abi.cpp). The stub stands in for
// amd_fidelityfx_framegeneration_dx12.dll.orig so the built Sedici DLL can be
// exercised through its five exports without AMD's runtime: it records what the
// proxy forwards and returns scripted results. It proves Sedici's side of the
// boundary only, never the behaviour of AMD's binary.

#include <cstdint>

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "ffx_api_framegeneration_dx12.h"

enum StubExport : uint32_t {
    StubCreate,
    StubDestroy,
    StubConfigure,
    StubQuery,
    StubDispatch,
    StubExportCount,
};

// A desc type no FFX header defines. Forwarding tests send it so that neither
// Sedici's routing nor its overrides recognise it.
constexpr uint64_t kStubProbeType = 0x7F5E0000ull;

// Probe desc: a header followed by a recognisable payload, 128 bytes in all so
// the stub can copy a fixed-size snapshot.
struct StubProbe {
    ffxApiHeader header;
    uint8_t payload[128 - sizeof(ffxApiHeader)];
};
static_assert(sizeof(StubProbe) == 128, "the stub snapshots exactly one probe");

struct StubCall {
    ffxContext* contextArg = nullptr;   // pointer the proxy passed
    void* contextValue = nullptr;       // *contextArg on entry
    const void* desc = nullptr;         // desc pointer the proxy passed
    uint64_t type = 0;                  // first desc type
    const void* callbacks = nullptr;    // create/destroy allocation callbacks
    uint8_t probe[sizeof(StubProbe)]{}; // bytes seen, when the desc is a probe
};

struct StubState {
    ffxReturnCode_t script[StubExportCount]{}; // returned by each export
    uint32_t calls[StubExportCount]{};
    StubCall last[StubExportCount]{};

    // Frame pacing tuning sends (swapchain key/value configure).
    uint32_t pacingSends = 0;
    FfxApiSwapchainFramePacingTuning tuning{};

    // Frame generation create flags and configure fields as forwarded.
    bool sawFgCreate = false;
    uint32_t fgCreateFlags = 0;
    bool sawFgConfigure = false;
    bool fgConfigureAllowAsync = false;
    bool fgConfigurePresentCallback = false;

    uintptr_t nextHandle = 0;
};

using StubStateFn = StubState* (*)();
