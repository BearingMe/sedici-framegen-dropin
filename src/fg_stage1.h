#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "fg_params.h"

// Stage 1 of Sedici's frame generation: the Mob-FGSR analytical pass.
//
// It takes the two most recent real frames (HUD-less color at display
// resolution, depth and motion vectors at render resolution) and produces one
// interpolated frame at the midpoint. Nothing here talks to FFX; the caller
// owns the command list, the frame history ordering and the output resource.
//
// Resource contract for one call:
//   * `color` is display resolution, `depth`/`motion` are render resolution.
//   * Every input is transitioned from `InputFrame::state` to the state each
//     pass needs, then back to that same state before the call returns.
//   * `output` is display resolution and is left in UNORDERED_ACCESS.

namespace sedici::fg {

struct InputFrame {
    ID3D12Resource* color = nullptr;   // display resolution, R8G8B8A8_UNORM
    ID3D12Resource* depth = nullptr;   // render resolution
    ID3D12Resource* motion = nullptr;  // render resolution, two or more float channels

    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Formats of the game's resources; the decode pass converts them to the
    // pipeline's internal layout, so any float depth/motion format works.
    DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
};

class Stage1 {
public:
    Stage1() = default;
    ~Stage1() = default;

    Stage1(const Stage1&) = delete;
    Stage1& operator=(const Stage1&) = delete;

    // Creates the root signature and pipelines. Device must outlive this object.
    bool initialize(ID3D12Device* device) noexcept;

    // Applies the frame geometry. Recreates the intermediate targets when the
    // sizes change; safe to call every frame.
    bool configure(const FrameParams& params) noexcept;

    // Returns false and leaves the command list untouched when the pipeline is
    // not configured or the inputs do not match the configured geometry.
    // `output` is display resolution and is returned to `outputState`.
    bool generate(ID3D12GraphicsCommandList* list, const InputFrame& frame,
                  ID3D12Resource* output, bool resetHistory,
                  D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON) noexcept;

    const FrameParams& params() const noexcept { return m_params; }
    bool ready() const noexcept { return m_device != nullptr && m_pipelines_ready; }

    // Releases every intermediate target. Keeps device and pipelines.
    void release_targets() noexcept;

private:
    struct Target {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;

        ID3D12Resource* get() const noexcept { return resource.Get(); }
    };

    bool compile_pipeline(const char* body, const char* name,
                          ID3D12PipelineState** out) noexcept;
    bool create_target(Target& target, DXGI_FORMAT format, uint32_t width, uint32_t height,
                       D3D12_RESOURCE_FLAGS flags) noexcept;
    bool create_targets() noexcept;

    void transition(ID3D12GraphicsCommandList* list, Target& target,
                    D3D12_RESOURCE_STATES after) noexcept;
    void transition_external(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept;
    void uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) noexcept;

    // Descriptor blocks: each pass owns one, because a table points at mutable
    // heap memory that the GPU reads when it executes, not when it records.
    void begin_block(uint32_t index) noexcept;
    void write_srv(uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) noexcept;
    void write_uav(uint32_t slot, ID3D12Resource* resource, DXGI_FORMAT format) noexcept;
    void copy_target(ID3D12GraphicsCommandList* list, Target& source, Target& destination) noexcept;

    bool dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline,
                  uint32_t groupsX, uint32_t groupsY) noexcept;

    ID3D12Device* m_device = nullptr;
    bool m_pipelines_ready = false;

    FrameParams m_params{};
    bool m_configured = false;
    uint32_t m_current = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root_signature;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_increment = 0;
    uint32_t m_block = 0;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_decode;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_dilate;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clear;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_reproject;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fill;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_warp;

    Target m_decodedDepth;
    Target m_decodedMotion;
    Target m_depth[2];
    Target m_motion[2];
    Target m_color[2];
    Target m_reprojection;
    Target m_filled;
};

} // namespace sedici::fg
