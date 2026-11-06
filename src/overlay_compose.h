#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace sedici {

// The copy AMD's default present callback performs when the application has
// not registered a separate UI resource: current back buffer -> output swap
// chain buffer, on the callback's command list, restoring both resources to
// their incoming states. Returns false when the resources are missing or when
// the call would be a no-op for an unsupported reason.
bool copy_back_buffer(ID3D12GraphicsCommandList* command_list,
                      ID3D12Resource* source,
                      uint32_t source_state,
                      ID3D12Resource* output,
                      uint32_t output_state) noexcept;

// Maps an FFX API resource state to the D3D12_RESOURCE_STATES value. Returns
// the value as uint32_t so callers outside this header do not need d3d12.h.
uint32_t dx12_state_from_ffx(uint32_t ffx_state) noexcept;

} // namespace sedici
