#pragma once

// HLSL for stage 1 of the frame generator.
//
// Translated from the upstream Mob-FGSR compute shaders
// (references/sources/mobfgsr/resources/FrameGeneration/*.comp and
// Preprocessing/Dilate.comp, CC BY-NC 4.0, see that licence). Differences:
//
// * GLSL image/texelFetch became DX12 Texture2D Load/UAV access.
// * Input motion arrives in pixels relative to motionVectorScale and is
//   converted to UV space once, in `dilate`.
// * Depth is remapped to the upstream "smaller is nearer" convention here.
// * `warp` samples the original 128x128 FLOAT atlas, exported offline with
//   tools/export_mobfgsr_lut.py. No EXR dependency exists at runtime.
//
// The kernels are embedded as source so the deployed DLL has no external
// shader files. They are compiled once per device at first use.

namespace sedici::fg::shaders {

inline constexpr const char* kCommon = R"hlsl(
#define INVALID 0xffffffffu

cbuffer Constants : register(b0)
{
    float4 renderSize;
    float4 presentationSize;
    float4 delta;
    float2 jitterOffset;
    float depthThreshold;
    float colorThreshold;
    float reserved;
    float depthScale;
    float depthBias;
    float renderScale;
    float2 motionVectorScale;
    float2 padding;
};

static const uint kDepthBits = 11u;
static const uint kXBits = 11u;
static const uint kYBits = 10u;
static const uint kMaxDepth = (1u << kDepthBits) - 1u;
static const int kMinX = -(1 << (kXBits - 1));
static const int kMinY = -(1 << (kYBits - 1));
static const int kMaxX = (1 << (kXBits - 1)) - 1;
static const int kMaxY = (1 << (kYBits - 1)) - 1;

uint pack_reprojection(float depth, int2 sourcePos, int2 targetPos)
{
    uint packedDepth = (uint)((float)kMaxDepth * saturate(depth));
    int2 relative = clamp(targetPos - sourcePos, int2(kMinX, kMinY), int2(kMaxX, kMaxY));
    uint2 unsignedRelative = (uint2)(relative - int2(kMinX, kMinY));
    return (packedDepth << (32 - kDepthBits)) | (unsignedRelative.x << kYBits) | unsignedRelative.y;
}

float unpack_depth(uint value)
{
    return (float)(value >> (32 - kDepthBits)) / (float)kMaxDepth;
}

int2 unpack_source(int2 targetPos, uint value)
{
    uint unsignedX = (value >> kYBits) & ((1u << kXBits) - 1u);
    uint unsignedY = value & ((1u << kYBits) - 1u);
    int2 relative = int2(unsignedX, unsignedY) + int2(kMinX, kMinY);
    return targetPos - relative;
}

// FFX depth is reversed-Z when DEPTH_INVERTED is set; the shaders want
// "smaller is nearer".
float remap_depth(float raw)
{
    return saturate(raw * depthScale + depthBias);
}

int2 clamp_pos(int2 pos, int2 size)
{
    return clamp(pos, int2(0, 0), max(size - 1, int2(0, 0)));
}

Buffer<float> sampleWeights : register(t7);

float lut_weight(float2 position)
{
    position = clamp(position, 0.0f, 127.0f);
    int2 p0 = (int2)floor(position);
    int2 p1 = min(p0 + 1, 127);
    float2 f = frac(position);
    return lerp(lerp(sampleWeights[p0.y * 128 + p0.x], sampleWeights[p0.y * 128 + p1.x], f.x),
                lerp(sampleWeights[p1.y * 128 + p0.x], sampleWeights[p1.y * 128 + p1.x], f.x), f.y);
}

// Same 4x4 weights and atlas coordinates as upstream Sample.glsl.
float4 sample_lut(Texture2D<float4> tex, float2 uv, float2 size)
{
    int2 texelSize = int2(size);
    float2 position = uv * size;
    float2 center = round(position);
    float2 fraction = position - center + 0.5f;
    int2 basePos = (int2)(center - 0.5f);

    float4 accumulator = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    [unroll] for (int y = 0; y < 4; ++y)
    {
        [unroll] for (int x = 0; x < 4; ++x)
        {
            float weight = lut_weight(31.0f * fraction + float2(x, y) * 32.0f);
            int2 samplePos = clamp_pos(basePos + int2(x - 1, y - 1), texelSize);
            accumulator += tex.Load(int3(samplePos, 0)) * weight;
            weightSum += weight;
        }
    }

    return abs(weightSum) > 1e-6f ? accumulator / weightSum : tex.Load(int3(clamp_pos(basePos, texelSize), 0));
}
)hlsl";

// Input decode: any float motion format becomes RG16F in UV units, depth is
// remapped to the upstream "smaller is nearer" convention. Kept separate from
// dilate so the pipeline never depends on the game's exact texture formats.
inline constexpr const char* kDecode = R"hlsl(
Texture2D<float> inputDepth : register(t0);
Texture2D<float4> inputMotion : register(t1);
RWTexture2D<float> decodedDepth : register(u0);
RWTexture2D<float2> decodedMotion : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;

    decodedDepth[pos] = remap_depth(inputDepth.Load(int3(pos, 0)));
    decodedMotion[pos] = inputMotion.Load(int3(pos, 0)).xy * motionVectorScale;
}
)hlsl";

// Depth and motion dilation: each pixel borrows the nearest (nearest to the
// camera) neighbour's motion so splatting does not leave gaps on thin geometry.
inline constexpr const char* kDilate = R"hlsl(
Texture2D<float> inputDepth : register(t0);
Texture2D<float2> inputMotion : register(t1);
RWTexture2D<float> outputDepth : register(u0);
RWTexture2D<float2> outputMotion : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;

    static const int2 offsets[8] =
    {
        int2(-1, -1), int2(-1, 0), int2(-1, 1), int2(0, -1),
        int2(0, 1), int2(1, -1), int2(1, 0), int2(1, 1)
    };

    int2 nearestPos = pos;
    float nearestDepth = inputDepth.Load(int3(pos, 0));

    [unroll] for (uint i = 0; i < 8; ++i)
    {
        int2 samplePos = clamp_pos(pos + offsets[i], size);
        float depth = inputDepth.Load(int3(samplePos, 0));
        if (depth < nearestDepth)
        {
            nearestDepth = depth;
            nearestPos = samplePos;
        }
    }

    outputDepth[pos] = nearestDepth;
    outputMotion[pos] = inputMotion.Load(int3(nearestPos, 0));
}
)hlsl";

// Reprojection buffer reset.
inline constexpr const char* kClear = R"hlsl(
RWTexture2D<uint> reprojection : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;
    reprojection[pos] = INVALID;
}
)hlsl";

// Quadratic forward splat with a depth test: the nearest surface wins.
inline constexpr const char* kReproject = R"hlsl(
Texture2D<float> currentDepth : register(t0);
Texture2D<float2> currentMotion : register(t1);
Texture2D<float2> previousMotion : register(t2);
RWTexture2D<uint> reprojection : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos1 = int2(id.xy);
    if (any(pos1 >= size)) return;

    float2 uv = ((float2)pos1 + 0.5f) * renderSize.zw;
    float2 mv1 = currentMotion.Load(int3(pos1, 0));

    // Motion of the surface point one frame earlier, used for the acceleration
    // term of the quadratic trajectory.
    int2 pos0 = clamp_pos((int2)((uv - mv1) * renderSize.xy), size);
    float2 mv0 = previousMotion.Load(int3(pos0, 0));

    float2 uvDelta = uv + (-1.0f + delta.y + delta.w) * mv1 + (delta.y - delta.w) * mv0;

    if (all(uvDelta >= 0.0f) && all(uvDelta <= 1.0f))
    {
        int2 posDelta = clamp_pos((int2)(uvDelta * renderSize.xy), size);
        float depth = currentDepth.Load(int3(pos1, 0));
        InterlockedMin(reprojection[posDelta], pack_reprojection(depth, pos1, posDelta));
    }
}
)hlsl";

// Thin-object hole filling: replace a missing projection with the nearest
// foreground neighbour unless the centre already matches a 2x2 block.
inline constexpr const char* kFill = R"hlsl(
Texture2D<uint> reprojection : register(t0);
RWTexture2D<uint> filled : register(u0);

#define SETBIT(x) (1u << x)
#define FILL_DEPTH_DIFF_THRESHOLD 0.0005f

void select_valid_neighbor(int2 centerPos, float centerDepth, uint index,
                           inout float nearestDepth, inout uint selected, inout uint mask)
{
    static const int2 offsets[9] =
    {
        int2(-1, -1), int2(-1, 0), int2(-1, 1),
        int2(0, -1), int2(0, 0), int2(0, 1),
        int2(1, -1), int2(1, 0), int2(1, 1)
    };

    int2 neighborPos = centerPos + offsets[index];
    if (any(neighborPos < int2(0, 0)) || any(neighborPos >= int2(renderSize.xy))) return;

    uint neighborData = reprojection.Load(int3(neighborPos, 0));
    bool neighborValid = neighborData != INVALID;
    float neighborDepth = unpack_depth(neighborData);

    if (neighborValid && (centerDepth - neighborDepth) > FILL_DEPTH_DIFF_THRESHOLD)
    {
        if (neighborDepth < nearestDepth)
        {
            nearestDepth = neighborDepth;
            selected = neighborData;
        }
    }
    else
    {
        mask |= SETBIT(index);
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;

    uint centerData = reprojection.Load(int3(pos, 0));
    float centerDepth = unpack_depth(centerData);
    float nearestDepth = 1.0f;
    uint selected = INVALID;
    uint mask = SETBIT(4);

    [unroll] for (uint i = 0; i < 8; ++i)
    {
        uint index = i < 4 ? i : i + 1;
        select_valid_neighbor(pos, centerDepth, index, nearestDepth, selected, mask);
    }

    const uint rejectionMasks[4] =
    {
        SETBIT(0) | SETBIT(1) | SETBIT(3) | SETBIT(4),
        SETBIT(1) | SETBIT(2) | SETBIT(4) | SETBIT(5),
        SETBIT(3) | SETBIT(4) | SETBIT(6) | SETBIT(7),
        SETBIT(4) | SETBIT(5) | SETBIT(7) | SETBIT(8)
    };

    bool reject = false;
    [unroll] for (uint m = 0; m < 4; ++m)
    {
        reject = reject || ((mask & rejectionMasks[m]) == rejectionMasks[m]);
    }

    uint result;
    if (reject)
    {
        result = centerData;
    }
    else
    {
        result = selected;
    }

    filled[pos] = result;
}
)hlsl";

// Backward warp of both real frames to the midpoint, then the depth and
// brightness based fusion from the paper.
inline constexpr const char* kWarp = R"hlsl(
Texture2D<uint> filledReprojection : register(t0);
Texture2D<float4> currentColor : register(t1);
Texture2D<float4> previousColor : register(t2);
Texture2D<float> currentDepth : register(t3);
Texture2D<float> previousDepth : register(t4);
Texture2D<float2> currentMotion : register(t5);
Texture2D<float2> previousMotion : register(t6);
RWTexture2D<float4> output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 displaySize = int2(presentationSize.xy);
    int2 renderTexelSize = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= displaySize)) return;

    float2 uv = ((float2)pos + 0.5f) * presentationSize.zw;
    int2 scaledPos = clamp_pos((int2)(uv * renderSize.xy), renderTexelSize);

    uint packedData = filledReprojection.Load(int3(scaledPos, 0));
    float2 mv1;
    int2 posBeforeReprojection = int2(-1, -1);

    if (packedData == INVALID)
    {
        mv1 = previousMotion.Load(int3(scaledPos, 0));
    }
    else
    {
        posBeforeReprojection = unpack_source(scaledPos, packedData);
        mv1 = currentMotion.Load(int3(clamp_pos(posBeforeReprojection, renderTexelSize), 0));
    }

    float2 sampleUV_t1;
    float2 sampleUV_t0;

    if (posBeforeReprojection.x < 0)
    {
        // No projection for this pixel: fall back to linear motion.
        sampleUV_t1 = uv + mv1 * (1.0f - delta.x);
        sampleUV_t0 = uv - mv1 * delta.x;
    }
    else
    {
        float2 uv_t1 = ((float2)posBeforeReprojection + 0.5f) * renderSize.zw;
        float2 uv_t0 = uv_t1 - mv1;
        int2 mvPos = clamp_pos((int2)(uv_t0 * renderSize.xy), renderTexelSize);
        float2 mv0 = previousMotion.Load(int3(mvPos, 0));
        sampleUV_t0 = uv + (-delta.y - delta.w) * mv1 + (-delta.y + delta.w) * mv0;
        sampleUV_t1 = mv1 + sampleUV_t0;
    }

    int2 samplePos_t1 = clamp_pos((int2)(sampleUV_t1 * renderSize.xy), renderTexelSize);
    int2 samplePos_t0 = clamp_pos((int2)(sampleUV_t0 * renderSize.xy), renderTexelSize);

    float4 color_t1 = sample_lut(currentColor, sampleUV_t1, presentationSize.xy);
    float4 color_t0 = sample_lut(previousColor, sampleUV_t0, presentationSize.xy);
    float depth_t1 = currentDepth.Load(int3(samplePos_t1, 0));
    float depth_t0 = previousDepth.Load(int3(samplePos_t0, 0));

    float3 color;
    float depthDiff = abs(depth_t0 - depth_t1);

    if (any(sampleUV_t0 < 0.0f) || any(sampleUV_t0 > 1.0f))
    {
        color = color_t1.rgb;
    }
    else if (any(sampleUV_t1 < 0.0f) || any(sampleUV_t1 > 1.0f))
    {
        color = color_t0.rgb;
    }
    else if (depthDiff < depthThreshold)
    {
        float3 colorDiff = abs(color_t1.rgb - color_t0.rgb);
        float lumaDiff = colorDiff.r * 0.5f + colorDiff.b * 0.5f + colorDiff.g;
        if (lumaDiff < colorThreshold)
        {
            color = delta.x < 0.5f ? color_t0.rgb : color_t1.rgb;
        }
        else
        {
            color = lerp(color_t0.rgb, color_t1.rgb, delta.x);
        }
    }
    else
    {
        // Different surfaces: keep the one further from the camera.
        color = depth_t1 > depth_t0 ? color_t1.rgb : color_t0.rgb;
    }

    output[pos] = float4(color, 1.0f);
}
)hlsl";

// HUD preservation for generated frames.
//
// The game's backbuffer carries the HUD; the HUD-less colour Sedici captured for
// the same frame is the same image without it. Their difference is the HUD layer,
// which is composited onto the generated midpoint. With no backbuffer supplied
// the weight is zero and this pass is a plain copy of the midpoint.
//
// Limitation: a difference over a moving background captures background change
// too, so a semi-transparent HUD over fast motion can leave a faint residue. A
// separately registered UI resource avoids it; this path exists for the observed
// Cyberpunk configuration, which registers none.
inline constexpr const char* kComposite = R"hlsl(
Texture2D<float4> midpoint : register(t0);
Texture2D<float4> hudFrame : register(t1);
Texture2D<float4> hudlessFrame : register(t2);
RWTexture2D<float4> output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(presentationSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;

    float4 generated = midpoint.Load(int3(pos, 0));
    float weight = saturate(padding.x);
    if (weight > 0.0f)
    {
        float3 withHud = hudFrame.Load(int3(pos, 0)).rgb;
        float3 withoutHud = hudlessFrame.Load(int3(pos, 0)).rgb;
        generated.rgb = saturate(generated.rgb + (withHud - withoutHud) * weight);
    }
    output[pos] = float4(generated.rgb, 1.0f);
}
)hlsl";

} // namespace sedici::fg::shaders
