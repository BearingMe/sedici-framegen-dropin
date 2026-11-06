#include "fg_params.h"

namespace sedici::fg {

namespace {
constexpr float kMinScale = 1.0f / 65536.0f;
}

bool valid(const FrameParams& params) noexcept
{
    if (params.renderWidth == 0 || params.renderHeight == 0) return false;
    if (params.displayWidth == 0 || params.displayHeight == 0) return false;
    if (!(params.alpha > 0.0f && params.alpha < 1.0f)) return false;
    if (params.motionVectorScaleX < kMinScale || params.motionVectorScaleY < kMinScale) return false;
    if (params.displayWidth < params.renderWidth || params.displayHeight < params.renderHeight) return false;
    return true;
}

Constants make_constants(const FrameParams& params) noexcept
{
    Constants c{};

    const float rw = static_cast<float>(params.renderWidth);
    const float rh = static_cast<float>(params.renderHeight);
    const float dw = static_cast<float>(params.displayWidth);
    const float dh = static_cast<float>(params.displayHeight);

    c.renderSize[0] = rw;
    c.renderSize[1] = rh;
    c.renderSize[2] = 1.0f / rw;
    c.renderSize[3] = 1.0f / rh;

    c.presentationSize[0] = dw;
    c.presentationSize[1] = dh;
    c.presentationSize[2] = 1.0f / dw;
    c.presentationSize[3] = 1.0f / dh;

    const float a = params.alpha;
    c.delta[0] = a;
    c.delta[1] = a * 0.5f;
    c.delta[2] = a * 1.5f;
    c.delta[3] = a * a * 0.5f;

    c.jitter[0] = 0.0f;
    c.jitter[1] = 0.0f;

    c.depthThreshold = params.depthThreshold;
    c.colorThreshold = params.colorThreshold;

    // The shader stores "smaller is nearer". Reversed-Z arrives the other way
    // round, so flip it instead of branching per pixel.
    c.depthScale = params.depthInverted ? -1.0f : 1.0f;
    c.depthBias = params.depthInverted ? 1.0f : 0.0f;

    c.renderScale = dw / rw;

    c.motionVectorScale[0] = params.motionVectorScaleX;
    c.motionVectorScale[1] = params.motionVectorScaleY;

    return c;
}

} // namespace sedici::fg
