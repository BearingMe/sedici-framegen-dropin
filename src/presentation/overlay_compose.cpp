#include "presentation/overlay_compose.h"

#include "ffx_api_types.h"

#include <d3d12.h>

namespace sedici {
namespace {

D3D12_RESOURCE_STATES to_dx12(uint32_t state) noexcept
{
    switch (state) {
    case FFX_API_RESOURCE_STATE_COMMON:
        return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case FFX_API_RESOURCE_STATE_PRESENT:
        return D3D12_RESOURCE_STATE_PRESENT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

void add_transition(D3D12_RESOURCE_BARRIER* barriers, UINT& count, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
{
    if (!resource || before == after) return;

    D3D12_RESOURCE_BARRIER& barrier = barriers[count++];
    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
}

} // namespace

uint32_t dx12_state_from_ffx(uint32_t ffx_state) noexcept
{
    return static_cast<uint32_t>(to_dx12(ffx_state));
}

bool copy_back_buffer(ID3D12GraphicsCommandList* command_list, ID3D12Resource* source, uint32_t source_state,
                      ID3D12Resource* output, uint32_t output_state) noexcept
{
    if (!command_list || !source || !output) return false;
    if (source == output) return true;

    const D3D12_RESOURCE_STATES source_before = to_dx12(source_state);
    const D3D12_RESOURCE_STATES output_before = to_dx12(output_state);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT count = 0;
    add_transition(barriers, count, source, source_before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add_transition(barriers, count, output, output_before, D3D12_RESOURCE_STATE_COPY_DEST);
    if (count != 0) command_list->ResourceBarrier(count, barriers);

    command_list->CopyResource(output, source);

    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_STATES before = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter = before;
    }
    if (count != 0) command_list->ResourceBarrier(count, barriers);
    return true;
}

} // namespace sedici
