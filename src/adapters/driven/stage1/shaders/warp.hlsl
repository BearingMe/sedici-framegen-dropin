// Backward warp of both real frames to the midpoint, then the depth and brightness based fusion
// from the paper.

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
