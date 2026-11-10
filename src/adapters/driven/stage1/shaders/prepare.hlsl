// Input decode and dilation in one pass (P4.4, K117). Any float motion format becomes RG16F in UV
// units and depth is remapped to the upstream "smaller is nearer" convention; then each pixel
// borrows the motion of its nearest (nearest to the camera) neighbour so splatting does not leave
// gaps on thin geometry. This replaces the two earlier passes (decode, then dilate) without
// changing a bit of the result: the decoded depth is the same float, and the motion is rounded to
// half once, as the old decode target did.

Texture2D<float> inputDepth : register(t0);
Texture2D<float4> inputMotion : register(t1);
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
    float nearestDepth = remap_depth(inputDepth.Load(int3(pos, 0)));

    [unroll] for (uint i = 0; i < 8; ++i)
    {
        int2 samplePos = clamp_pos(pos + offsets[i], size);
        float depth = remap_depth(inputDepth.Load(int3(samplePos, 0)));
        if (depth < nearestDepth)
        {
            nearestDepth = depth;
            nearestPos = samplePos;
        }
    }

    outputDepth[pos] = nearestDepth;
    outputMotion[pos] = inputMotion.Load(int3(nearestPos, 0)).xy * motionVectorScale;
}
