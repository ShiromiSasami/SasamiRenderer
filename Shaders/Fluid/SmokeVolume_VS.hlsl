// SmokeVolume_VS.hlsl
// Fullscreen triangle generated purely from SV_VertexID -- no vertex or index buffer, same
// bit-trick as VolumetricCloud_VS.hlsl and RayMarch_VS-style passes elsewhere in this renderer.
//
// The clip-space z written here is a placeholder (far plane under this reverse-Z renderer,
// near=1/far=0). It does not decide occlusion: SmokeVolume_PS.hlsl writes its own per-pixel
// depth via SV_Depth, reprojected from the ray/box intersection, because a single flat triangle
// cannot otherwise carry a depth that varies across the volume's footprint. See SmokeVolume_PS's
// header comment for why that per-pixel depth is required.

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    // Generate a fullscreen triangle from three vertices using bit-tricks:
    //   id=0 → (-1, -1)
    //   id=1 → ( 3, -1)
    //   id=2 → (-1,  3)
    float2 uv  = float2((id << 1u) & 2u, id & 2u);
    float2 ndc = uv * 2.0f - 1.0f;

    VSOutput o;
    o.position = float4(ndc.x, -ndc.y, 0.0f, 1.0f); // placeholder depth; PS overrides via SV_Depth
    o.uv       = uv;
    return o;
}
