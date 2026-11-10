// HUD preservation for generated frames.
//
// The game's backbuffer carries the HUD; the HUD-less colour Sedici captured for the same frame is
// the same image without it. Their difference is the HUD layer, which is composited onto the
// generated midpoint. With no backbuffer supplied the weight is zero and this pass is a plain copy
// of the midpoint.
//
// Limitation: a difference over a moving background captures background change too, so a
// semi-transparent HUD over fast motion can leave a faint residue. A separately registered UI
// resource avoids it; this path exists for the observed Cyberpunk configuration, which registers
// none.

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
