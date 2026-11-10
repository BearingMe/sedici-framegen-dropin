// Reprojection buffer reset.

RWTexture2D<uint> reprojection : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int2 size = int2(renderSize.xy);
    int2 pos = int2(id.xy);
    if (any(pos >= size)) return;
    reprojection[pos] = INVALID;
}
