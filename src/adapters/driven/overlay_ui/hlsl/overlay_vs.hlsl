// Vertex shader for the egui overlay's D3D12 renderer.
//
// egui gives positions in points (logical pixels, top-left origin) and premultiplied sRGB colours.
// The renderer uploads exactly the vertex buffer layout the C++ ImGui backend used:
//
//   float2 position    // points
//   float2 uv          // texture coordinates
//   uint4  color       // sRGBA, premultiplied, 8 bits per channel
//
// The root constants hold the output size in physical pixels, so points convert to clip space. The
// same conversion as ImGui's `imgui-impl-dx12.hlsl`: the top-left of the output is (-1, 1).

cbuffer OverlayConstants : register(b0)
{
    float2 screen_size;    // output width/height in pixels
    float2 inv_screen_size; // 1/width, 1/height in pixels
    float4 padding;
};

struct VSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(
        input.position.x * inv_screen_size.x * 2.0f - 1.0f,
        1.0f - input.position.y * inv_screen_size.y * 2.0f,
        0.0f,
        1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
