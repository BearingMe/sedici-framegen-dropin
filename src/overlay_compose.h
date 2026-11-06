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

} // namespace sedici
