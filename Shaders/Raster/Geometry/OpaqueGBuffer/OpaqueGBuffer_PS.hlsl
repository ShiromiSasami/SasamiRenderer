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
    // .x = transparentShellStrength, .y = doubleSided (>0.5 flips back-face normals), .zw unused.
    float4 u_materialTransparencyParams;
}

Texture2D AlbedoTex    : register(t0);
Texture2D OcclusionTex : register(t7);
// Material tangent-space normal map; flat (0.5,0.5,1) fallback when the material has none.
// t17 rather than a low register: the shared raster root signature already covers t0-t16
// (the IBL table alone spans t4-t6).
Texture2D NormalTex    : register(t17);
Texture2D EmissiveTex  : register(t18);
// SDF face-shadow threshold map (scene-wide, R channel). White when no map is set.
Texture2D FaceShadowTex : register(t19);
SamplerState LinearWrap : register(s0);

#include "Shared/Common/LightCB.hlsli"
#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"
#include "Raster/Geometry/OpaqueGBuffer/GBufferEncode.hlsli"
#include "Raster/Geometry/OpaqueGBuffer/ToonFaceShadow.hlsli"

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

GBufferOutput PSMain(PSInput i, bool isFrontFace : SV_IsFrontFace)
{
    GBufferSurface s;
    s.worldN      = i.worldN;
    s.worldPos    = i.worldPos;
    s.uv          = i.uv;
    s.color       = i.color;
    s.isFrontFace = isFrontFace;
    s.cameraPos   = u_cameraPos.xyz;

    GBufferMaterial m;
    m.baseColor         = u_materialBaseColor;
    m.emissiveRoughness = u_materialEmissiveRoughness;
    m.params            = u_materialParams;
    m.specularWorkflow  = u_materialSpecularWorkflow;
    m.reflectionParams  = u_materialReflectionParams;
    m.alphaCutoff       = u_materialTransparencyParams.z;

    GBufferOutput o = EncodeGBuffer(s, m, AlbedoTex, OcclusionTex, NormalTex, EmissiveTex, LinearWrap);

    // SDF face shadow (ToonFaceShadow.hlsli); the face frame comes from LightCB.
    if (u_materialTransparencyParams.w > 0.5 && u_toonFaceRight.w > 0.5 && IsToonShading(m.specularWorkflow.w)) {
        const float lit = ToonFaceShadowLit(normalize(-u_dirDir.xyz), u_toonFaceForward.xyz, u_toonFaceRight.xyz,
                                            u_toonFaceForward.w, i.uv, FaceShadowTex, LinearWrap);
        o.specularWorkflow = float4(lit, 0.0, 0.0, 0.375);
    }
    return o;
}
