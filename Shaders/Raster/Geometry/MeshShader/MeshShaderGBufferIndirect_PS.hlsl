// MeshShaderGBufferIndirect_PS.hlsl - GBuffer pixel shader for the GPU-driven (ExecuteIndirect)
// cluster path. Pairs with MeshShaderGBufferIndirect_MS.hlsl. There is no per-draw material
// constant buffer: the material and its bindless texture indices are fetched per pixel from
// the cluster material table, indexed by the instance index forwarded from the mesh shader.
// Reads: b0=CameraCB (from Common), t4=GpuClusterMaterial[] (per instance),
//        bindless Texture2D SRVs via ResourceDescriptorHeap, s0=LinearWrap
// Outputs: GBufferOutput (see GBufferEncode.hlsli)

#include "Raster/Geometry/MeshShader/MeshShader_Common.hlsli"

// Must match ClusterMaterialTable.h GpuClusterMaterial (144 bytes). extra[0..6] are the
// MaterialCB fields in order: baseColor, emissiveRoughness, params, specularWorkflow,
// reflectionParams, volumeParams, transparencyParams.
struct GpuClusterMaterial
{
    float4 extra[7];
    uint   albedoSrv;
    uint   occlusionSrv;
    uint   normalSrv;
    uint   flags;
    uint   emissiveSrv;
    uint3  pad;
};

StructuredBuffer<GpuClusterMaterial> g_materials : register(t4);
SamplerState LinearWrap : register(s0);

#include "Raster/Geometry/OpaqueGBuffer/GBufferEncode.hlsli"
#include "Raster/Geometry/OpaqueGBuffer/ToonFaceShadow.hlsli"

// Must match MeshShaderGBufferIndirect_MS.hlsl PSInput
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
    nointerpolation uint2 clusterInfo : TEXCOORD3; // x = page offset + cluster in page, y = owning group
    nointerpolation uint instanceIndex : TEXCOORD4;
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

// Same null-SRV fallback the CPU path binds for a missing texture. NonUniformResourceIndex
// is required because one wave can shade pixels of several instances.
Texture2D FetchMaterialTexture(uint srv)
{
    const uint idx = (srv != 0xFFFFFFFFu) ? srv : g_nullTextureSrvIndex;
    Texture2D tex = ResourceDescriptorHeap[NonUniformResourceIndex(idx)];
    return tex;
}

GBufferOutput PSMain(PSInput i, bool isFrontFace : SV_IsFrontFace)
{
    GpuClusterMaterial mat = g_materials[i.instanceIndex];

    Texture2D albedoTex    = FetchMaterialTexture(mat.albedoSrv);
    Texture2D occlusionTex = FetchMaterialTexture(mat.occlusionSrv);
    Texture2D normalTex    = FetchMaterialTexture(mat.normalSrv);
    Texture2D emissiveTex  = FetchMaterialTexture(mat.emissiveSrv);

    GBufferSurface s;
    s.worldN      = i.worldN;
    s.worldPos    = i.worldPos;
    s.uv          = i.uv;
    s.color       = i.color;
    s.isFrontFace = isFrontFace;
    s.cameraPos   = g_cameraPos;

    GBufferMaterial m;
    m.baseColor         = mat.extra[0];
    m.emissiveRoughness = mat.extra[1];
    m.params            = mat.extra[2];
    m.specularWorkflow  = mat.extra[3];
    m.reflectionParams  = mat.extra[4];
    m.alphaCutoff       = mat.extra[6].z;

    GBufferOutput o = EncodeGBuffer(s, m, albedoTex, occlusionTex, normalTex, emissiveTex, LinearWrap);

    // SDF face shadow, same rule as OpaqueGBuffer_PS (ToonFaceShadow.hlsli). extra[6].w is the flag.
    if (mat.extra[6].w > 0.5 && g_toonFaceEnabled > 0.5 && IsToonShading(m.specularWorkflow.w)) {
        const float lit = ToonFaceShadowLit(g_toonFaceToLight, g_toonFaceForward, g_toonFaceRight,
                                            g_toonFaceSoftness, i.uv, FetchMaterialTexture(g_toonFaceMapSrvIndex),
                                            LinearWrap);
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
