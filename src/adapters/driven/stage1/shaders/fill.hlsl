// Thin-object hole filling: replace a missing projection with the nearest foreground neighbour
// unless the centre already matches a 2x2 block.

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
