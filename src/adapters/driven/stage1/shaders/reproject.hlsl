// Quadratic forward splat with a depth test: the nearest surface wins.

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

    // Motion of the surface point one frame earlier, used for the acceleration term of the
    // quadratic trajectory.
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
