// Pixel shader for the egui overlay's D3D12 renderer.
//
// The texture is egui's font/glyph atlas in R8G8B8A8_UNORM. Colours arrive premultiplied from the
// vertex stream (that is how epaint stores `Color32`), the atlas is straight alpha, so the final
// colour is `vertex.rgb * tex.rgb` and the final alpha `vertex.a * tex.a`, exactly what ImGui's DX12
// backend writes and what the render target's `SRC_ALPHA = One, INV_SRC_ALPHA` blend then
// composites. Keep that multiply: a straight-alpha blend would double-darken every translucent
// panel. Untextured primitives use `WHITE_UV`, so multiplying by the atlas is correct for them too.

Texture2D overlay_texture : register(t0);
SamplerState overlay_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color * overlay_texture.Sample(overlay_sampler, input.uv);
}
