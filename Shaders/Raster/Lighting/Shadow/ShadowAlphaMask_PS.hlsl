// ShadowAlphaMask_PS.hlsl
// Alpha test for glTF alphaMode MASK materials in the depth-only shadow passes
// (glTF 2.0 3.9.6: a MASK texel is either fully opaque or discarded). Without it the
// shadow map keeps the whole quad, so cut-out hair, lashes and leaves cast solid
// rectangles even though the G-Buffer already clips them.
//
// Paired with the G-Buffer vertex shaders (CookTorranceGGX_VS / SkinnedMesh_VS), so the
// input struct must match their output. Only MASK draw items use this PS; every other
// shadow caster stays on the position-only, PS-less pipeline (ShadowDepth_VS).
//
// Same coverage formula as GBufferEncode.hlsli: albedo.a * vertex color.a * baseColor.a.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
    float4 u_materialSpecularWorkflow;
    float4 u_materialReflectionParams;
    float4 u_materialVolumeParams;
    // .z = MASK alpha cutoff (the same value OpaqueGBuffer_PS reads).
    float4 u_materialTransparencyParams;
}

Texture2D AlbedoTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

void PSMain(PSInput i)
{
    const float alpha = saturate(AlbedoTex.Sample(LinearWrap, i.uv).a * i.color.a * u_materialBaseColor.a);
    clip(alpha - u_materialTransparencyParams.z);
}
