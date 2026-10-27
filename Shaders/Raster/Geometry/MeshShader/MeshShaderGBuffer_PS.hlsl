// GBuffer pixel shader for the amplification/mesh shader path. Pairs with
// MeshShader_AS/MS; root signature in RenderPipelineStateCacheMeshShader.cpp.

// b0 (visibility ALL, shared with AS/MS)
#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

// b2 (PIXEL): the same per-draw constant block the VS path binds at b0
// (RendererFrameCoordinator.cpp CameraCBData: mvp, world, 7 float4 extras). Same member order/names
// as OpaqueGBuffer_PS.hlsl so the existing per-draw upload slot can be bound here unchanged.
cbuffer MaterialCB : register(b2)
{
    row_major float4x4 u_mvp;      // unused here; keeps the layout identical
    row_major float4x4 u_world;    // unused here
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
    float4 u_materialSpecularWorkflow;
    float4 u_materialReflectionParams;
    float4 u_materialVolumeParams;
    float4 u_materialTransparencyParams;
}

Texture2D AlbedoTex    : register(t3);
Texture2D OcclusionTex : register(t4);
Texture2D NormalTex    : register(t5);
Texture2D EmissiveTex  : register(t8);
// SDF face-shadow threshold map (scene-wide, R channel; root [12]). White when no map is set.
Texture2D FaceShadowTex : register(t9);
SamplerState LinearWrap : register(s0);

#include "Raster/Geometry/OpaqueGBuffer/GBufferEncode.hlsli"
#include "Raster/Geometry/OpaqueGBuffer/ToonFaceShadow.hlsli"

// Must match MeshShader_MS.hlsl's PSInput
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    // Cluster provenance for the debug views (g_clusterDebugMode). nointerpolation because
    // these are integers and constant across the whole cluster.
    nointerpolation uint2 clusterInfo : TEXCOORD3; // x = absolute cluster index, y = owning group
};

// Debug colouring for the virtual-geometry views. A cheap integer hash spreads adjacent
// ids across the wheel so neighbouring clusters never share a colour.
float3 ClusterDebugColor(uint id)
{
    uint h = id * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return float3((h & 0xFFu) / 255.0, ((h >> 8) & 0xFFu) / 255.0, ((h >> 16) & 0xFFu) / 255.0);
}

GBufferOutput PSMain(PSInput i, bool isFrontFace : SV_IsFrontFace)
{
    GBufferSurface s;
    s.worldN      = i.worldN;
    s.worldPos    = i.worldPos;
    s.uv          = i.uv;
    s.color       = i.color;
    s.isFrontFace = isFrontFace;
    s.cameraPos   = g_cameraPos;

    GBufferMaterial m;
    m.baseColor         = u_materialBaseColor;
    m.emissiveRoughness = u_materialEmissiveRoughness;
    m.params            = u_materialParams;
    m.specularWorkflow  = u_materialSpecularWorkflow;
    m.reflectionParams  = u_materialReflectionParams;
    m.alphaCutoff       = u_materialTransparencyParams.z;

    GBufferOutput o = EncodeGBuffer(s, m, AlbedoTex, OcclusionTex, NormalTex, EmissiveTex, LinearWrap);

    // SDF face shadow, same rule as OpaqueGBuffer_PS (ToonFaceShadow.hlsli).
    if (u_materialTransparencyParams.w > 0.5 && g_toonFaceEnabled > 0.5 && IsToonShading(m.specularWorkflow.w)) {
        const float lit = ToonFaceShadowLit(g_toonFaceToLight, g_toonFaceForward, g_toonFaceRight,
                                            g_toonFaceSoftness, i.uv, FaceShadowTex, LinearWrap);
        o.specularWorkflow = float4(lit, 0.0, 0.0, 0.375);
    }

    // Debug views replace only the albedo target, so normals, depth and the material
    // channels stay valid and lighting keeps working on top of the flat colour.
    if (g_clusterDebugMode > 0.5f) {
        const uint id = (g_clusterDebugMode < 1.5f) ? i.clusterInfo.x : i.clusterInfo.y;
        o.albedo = float4(ClusterDebugColor(id), o.albedo.a);
    }

    return o;
}
