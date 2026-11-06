#pragma once

#include "ffx_api.h"

#include <d3d12.h>

#include "fg_params.h"
#include "fg_stage1.h"

// Sedici's frame generation context.
//
// Experimental FFX desc adapter, not yet an export provider. Supports the
// observed SDR, render-resolution-motion subset. It never calls the original
// DLL. A false result is a local rejection, NOT permission to forward a
// Sedici-owned context to AMD or to abandon an already recorded command list.
//
// Deliberately absent for now: the swapchain wrapper, presentation scheduling
// and UI composition. This type only knows how to turn FFX inputs into pixels.

namespace sedici::fg {

class Context {
public:
    // ffxCreateContext: FrameGenerationCreate (+ HUDLESS, BACKEND_DX12).
    bool create(const ffxApiHeader* createDesc) noexcept;
    // Refuses destruction while recorded/submitted GPU work has not retired.
    // The owner must also retire work before the C++ object's lifetime ends.
    bool destroy() noexcept;

    // ffxConfigure: FrameGenerationConfig.
    bool configure(const ffxApiHeader* configureDesc) noexcept;

    // ffxDispatch: FrameGenerationPrepare / V2.
    bool prepare(const ffxApiHeader* prepareDesc) noexcept;

    // Generates the pending midpoint into `output`. Returns false when the game
    // has not supplied everything a frame needs.
    bool generate(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
                  D3D12_RESOURCE_STATES outputState) noexcept;

    // After ExecuteCommandLists, register a queue-signalled fence covering ALL
    // work recorded since the last registration. No CPU-signalled completion.
    // For split prepare/generate submissions, order generation with wait_prepare
    // before executing it, then register its own completion fence.
    bool track_submission(ID3D12Fence* fence, uint64_t value) noexcept;
    bool wait_prepare(ID3D12CommandQueue* queue) noexcept;
    bool retired() const noexcept;

    bool ready() const noexcept { return m_stage.ready() && m_display_width != 0; }
    bool enabled() const noexcept { return m_enabled; }
    bool pending() const noexcept { return m_prepare_valid; }
    bool have_history() const noexcept { return m_history_valid; }
    uint64_t frames_generated() const noexcept { return m_frames_generated; }
    uint64_t frame_id() const noexcept { return m_frame_id; }
    const char* last_error() const noexcept { return m_error; }

private:
    bool fail(const char* reason) noexcept;
    void reset_history() noexcept { m_history_valid = false; }

    ID3D12Device* m_device = nullptr;
    uint32_t m_create_flags = 0;
    uint32_t m_display_width = 0;
    uint32_t m_display_height = 0;
    uint32_t m_max_render_width = 0;
    uint32_t m_max_render_height = 0;

    bool m_enabled = false;
    bool m_prepare_valid = false;
    bool m_history_valid = false;
    bool m_reset_requested = true;

    uint64_t m_frame_id = 0;
    uint64_t m_prepared_frame_id = 0;
    uint64_t m_frames_generated = 0;

    D3D12_RESOURCE_STATES m_hudless_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* m_hudless = nullptr;

    D3D12_RESOURCE_STATES m_depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES m_motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* m_depth = nullptr;
    ID3D12Resource* m_motion = nullptr;

    uint32_t m_render_width = 0;
    uint32_t m_render_height = 0;
    float m_motion_scale_x = 1.0f;
    float m_motion_scale_y = 1.0f;

    Stage1 m_stage;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_retirement_fence;
    uint64_t m_retirement_value = 0;
    bool m_recorded = false;
    bool m_snapshot = false;
    bool m_prepare_waited = false;
    ID3D12GraphicsCommandList* m_prepare_list = nullptr; // identity only, never dereferenced
    const char* m_error = "";
};

} // namespace sedici::fg
