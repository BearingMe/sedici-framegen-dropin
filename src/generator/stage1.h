#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "generator/params.h"

// Stage 1 of Sedici's frame generation: the Mob-FGSR analytical pass.
//
// It takes the two most recent real frames (HUD-less color at display
// resolution, depth and motion vectors at render resolution) and produces one
// interpolated frame at the midpoint, or at any phase alpha in (0, 1). Nothing here talks to FFX; the caller
// owns the command list, the frame history ordering and the output resource.
//
// Resource contract for one call:
//   * `color` is display resolution, `depth`/`motion` are render resolution.
//   * Every input is transitioned from `InputFrame::state` to the state each
//     pass needs, then back to that same state before the call returns.
//   * `output` is display resolution and is restored to outputState.
// The owner must retire submitted work before configure/release/generate reuse.
// A Stage1 instance is a single in-flight slot, not a concurrent recorder.

namespace sedici::fg {

struct InputFrame {
    ID3D12Resource* color = nullptr;   // display resolution, FrameParams::colorFormat
    ID3D12Resource* depth = nullptr;   // render resolution
    ID3D12Resource* motion = nullptr;  // render resolution, two or more float channels

    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Typed SRV formats compatible with the game's resources, not necessarily
    // their allocation formats (e.g. D32_FLOAT depth is viewed as R32_FLOAT;
    // R32G8X24_TYPELESS uses R32_FLOAT_X8X24_TYPELESS on depth plane 0).
    // The decode pass converts these views to the pipeline's internal layout.
    DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_FLOAT;
    DXGI_FORMAT motionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
};

// Optional HUD source for a generated frame. The wrapper passes the game's
// current backbuffer, which still carries the HUD; Stage1 already holds the
// HUD-less copy it made for the same frame, so the difference is the HUD layer.
struct HudSource {
    ID3D12Resource* real = nullptr;
    D3D12_RESOURCE_STATES realState = D3D12_RESOURCE_STATE_PRESENT;
};

// Most generated frames recorded for one real frame. Two covers every output
// rate up to twice the real rate, including an adaptive target whose output
// slots do not line up with real frames.
constexpr uint32_t kMaxPhases = 2;

// One generated frame: `alpha` in (0, 1) is its time between the previous real
// frame (0) and the current one (1). `output` is returned to `outputState`.
struct GeneratePhase {
    ID3D12Resource* output = nullptr;
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
    float alpha = kMidpointAlpha;
};

// Profiling hook (P4.4, K117): called right after each compute pass is recorded so a
// test host can write a GPU timestamp there. `pass` is a string literal
// ("prepare", "clear", "reproject", "fill", "warp", "composite"). Null
// (the default) costs one pointer load per pass; production never sets it.
using PassMarkFn = void (*)(void* user, ID3D12GraphicsCommandList* list, const char* pass);
void stage1_set_pass_mark(PassMarkFn fn, void* user) noexcept;

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

    // Record depth/motion normalization and dilation on the game's prepare
    // command list, before the application can recycle its input textures.
    // Generation must execute after this command list on the same queue, or
    // with an explicit queue dependency. One outstanding pair per Stage1.
    bool capture_prepare(ID3D12GraphicsCommandList* list, const InputFrame& frame) noexcept;

    // Only after all recorded work has retired. Keep resources/state tracking,
    // but abandon the snapshot and force the next generated frame to seed history.
    void discard_prepare() noexcept;

    // Returns false and leaves the command list untouched when the pipeline is
    // not configured or the inputs do not match the configured geometry.
    // `output` is display resolution and is returned to `outputState`.
    // With `hud`, the HUD layer of `hud->real` is composited onto the midpoint.
    bool generate(ID3D12GraphicsCommandList* list, const InputFrame& frame,
                  ID3D12Resource* output, bool resetHistory,
                  D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON,
                  const HudSource* hud = nullptr) noexcept;

    // Reset seed contract (K53). When history is reset or not yet valid there is
    // no temporal pair, so every output receives the REAL frame: the game's own
    // final frame (HUD included) when a HudSource is given, otherwise the
    // HUD-less colour. A seed is therefore not a HUD-less phase, and a future
    // residual stage must run only on the synthesize() path, never on a seed.
    //
    // Same, for `count` (1..kMaxPhases) generated frames between the same pair
    // of real frames, each at its own alpha into its own output. The history
    // advances once. On a reset every output receives the real frame.
    bool generate(ID3D12GraphicsCommandList* list, const InputFrame& frame,
                  const GeneratePhase* phases, uint32_t count, bool resetHistory,
                  const HudSource* hud = nullptr) noexcept;

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
    void synthesize(ID3D12GraphicsCommandList* list, uint32_t current, uint32_t previous,
                    float alpha, uint32_t block) noexcept;
    void composite_hud(ID3D12GraphicsCommandList* list, uint32_t current,
                       const GeneratePhase& phase, const HudSource* hud, uint32_t block) noexcept;

    ID3D12Device* m_device = nullptr;
    bool m_pipelines_ready = false;

    FrameParams m_params{};
    bool m_configured = false;
    bool m_history_valid = false;
    bool m_prepared = false;
    uint32_t m_current = 0;
    float m_composite_weight = 0.0f;
    float m_alpha = kMidpointAlpha; // phase written into the root constants

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root_signature;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lut;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    uint32_t m_increment = 0;
    uint32_t m_block = 0;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_prepare; // decode + dilate, one pass
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clear;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_reproject;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_fill;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_warp;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_composite;

    Target m_depth[2];
    Target m_motion[2];
    Target m_color[2];
    Target m_reprojection;
    Target m_filled;
    Target m_midpoint;
};

} // namespace sedici::fg
