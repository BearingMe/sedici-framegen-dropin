#pragma once

#include <cstdint>

// Stage 1 (Mob-FGSR analytical frame generation) constants and parameters.
//
// The first 80 bytes mirror the upstream constant block
// (references/sources/mobfgsr/resources/Uniforms.glsl) so the two can be
// compared term by term; the trailing five values are Sedici's extension for
// FFX inputs. The whole block is uploaded as root constants.

namespace sedici::fg {

// Interpolation phase for one generated frame between two real frames.
constexpr float kMidpointAlpha = 0.5f;

// Upstream defaults (references/sources/mobfgsr/src/offscreen_renderer.h).
// The depth threshold is not directly comparable to a reversed-Z buffer; see
// docs/findings/007-fg-baseline.md.
constexpr float kDefaultDepthThreshold = 0.004f;
constexpr float kDefaultColorThreshold = 0.01f;

struct FrameParams {
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;

    // Reverse-Z depth: near = 1, far = 0 (FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED).
    bool depthInverted = true;

    float alpha = kMidpointAlpha;
    float depthThreshold = kDefaultDepthThreshold;
    float colorThreshold = kDefaultColorThreshold;

    // Pixels of motion for a full-screen move, as reported in
    // ffxDispatchDescFrameGenerationPrepare::motionVectorScale.
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
};

// 96 bytes: the upstream 80-byte block plus the motion-vector scale.
struct Constants {
    float renderSize[4];       // width, height, 1/width, 1/height
    float presentationSize[4]; // width, height, 1/width, 1/height
    float delta[4];            // alpha, alpha/2, 3*alpha/2, alpha^2/2
    float jitter[2];           // reserved, upstream parity
    float depthThreshold;      // upstream depth_diff_threshold_fg
    float colorThreshold;      // upstream color_diff_threshold_fg
    float reserved;            // upstream depth_diff_threshold_sr
    float depthScale;
    float depthBias;
    float renderScale;
    float motionVectorScale[2];
    float padding[2];
};

static_assert(sizeof(Constants) == 96, "constant block must match the shader layout");

// Returns false when the sizes or parameters cannot describe a frame.
bool valid(const FrameParams& params) noexcept;

Constants make_constants(const FrameParams& params) noexcept;

} // namespace sedici::fg
