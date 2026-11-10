// Shared HLSL for stage 1 of the frame generator.
//
// Translated from the upstream Mob-FGSR compute shaders
// (references/sources/mobfgsr/resources/FrameGeneration/*.comp and
// Preprocessing/Dilate.comp, CC BY-NC 4.0, see that licence). Differences:
//
// * GLSL image/texelFetch became DX12 Texture2D Load/UAV access.
// * Input motion arrives in pixels relative to motionVectorScale and is converted to UV space
//   once, in `prepare`.
// * Depth is remapped to the upstream "smaller is nearer" convention here.
// * `warp` samples the original 128x128 FLOAT atlas, exported offline with
//   tools/export_mobfgsr_lut.py. No EXR dependency exists at runtime.
//
// This header is prepended to every pass source before compilation, exactly as the C++
// concatenated shaders::kCommon with each body.

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

// FFX depth is reversed-Z when DEPTH_INVERTED is set; the shaders want "smaller is nearer".
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
