// CrowdVat_PS.hlsl
// Writes the instanced VAT crowd into the GBuffer with a constant per-draw material.
//
// Root signature (CrowdVat):
//   [0] b0 : CrowdCB — VS + PS visibility
//            g_extra1 = (cameraPos.xyz, modelYOffset)
//            g_extra2 = (albedo.rgb linear, roughness)

#include "Raster/Geometry/OpaqueGBuffer/GBufferEncode.hlsli"

cbuffer CrowdCB : register(b0)
{
    row_major float4x4 g_viewProj;      // Camera view * projection
    row_major float4x4 g_worldUnused;   // Unused (kept for PushCameraCB layout compat)
    float4             g_extra0;        // Used by the VS only
    float4             g_extra1;        // .xyz = cameraPos
    float4             g_extra2;        // .rgb = albedo, .w = roughness
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float3 worldPos : TEXCOORD0;
};

GBufferOutput PSMain(VSOutput i, bool isFrontFace : SV_IsFrontFace)
{
    float3 N = normalize(i.worldN);
    if (!isFrontFace)
    {
        N = -N;
    }

    GBufferOutput o;
    o.albedo           = float4(saturate(g_extra2.rgb), 1.0f);
    o.normal           = float4(N * 0.5f + 0.5f, length(i.worldPos - g_extra1.xyz));
    o.material         = float4(saturate(g_extra2.w), 0.0f, 1.0f, 0.0f);
    o.emissive         = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.specularWorkflow = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return o;
}
