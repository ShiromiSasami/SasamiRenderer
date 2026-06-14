// MeshShader_AS.hlsl - Amplification Shader: per-meshlet frustum culling
// Each thread group processes one meshlet.
// Reads: t0=MeshletDesc[], b0=CameraCB, b1=DrawCB
// Dispatches one MS group per visible meshlet.

struct MeshletDesc
{
    uint   indexOffset;
    uint   indexCount;
    float3 boundsCenter;
    float  boundsRadius;
};

struct DrawCBData
{
    row_major float4x4 model;
    row_major float4x4 inverseModel;
    uint     meshletOffset; // First global meshlet index for this draw call
    uint     meshletCount;
    uint     pad0;
    uint     pad1;
};

cbuffer CameraCB : register(b0)
{
    row_major float4x4 g_viewProj;
    row_major float4x4 g_proj;
    float3   g_cameraPos;
    float    g_cameraPad;
}

cbuffer DrawCB : register(b1)
{
    DrawCBData g_draw;
}

StructuredBuffer<MeshletDesc> g_meshletDescs : register(t0);

struct ASPayload
{
    uint     meshletIndex;  // global meshlet index
    uint     lodLevel;      // 0=full, 1=half, 2=quarter triangle count
    uint     pad0;
    uint     pad1;
    row_major float4x4 model;
    row_major float4x4 inverseModel;
};

// Check if a world-space sphere is inside the view frustum.
// Planes extracted from the combined VP matrix (Gribb-Hartmann).
bool IsSphereVisible(float3 center, float radius, row_major float4x4 vp)
{
    // Row-major access: vp[row][col]
    float4 planes[6];
    // left
    planes[0] = float4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0],
                       vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    // right
    planes[1] = float4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0],
                       vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    // bottom
    planes[2] = float4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1],
                       vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    // top
    planes[3] = float4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1],
                       vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    // near
    planes[4] = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    // far
    planes[5] = float4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2],
                       vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float len  = length(planes[i].xyz);
        float dist = dot(planes[i].xyz, center) + planes[i].w;
        if (dist < -radius * len)
            return false;
    }
    return true;
}

[numthreads(1, 1, 1)]
void AS_Meshlet(uint dtid : SV_DispatchThreadID)
{
    ASPayload payload;
    payload.meshletIndex  = 0u;
    payload.lodLevel      = 0u;
    payload.pad0          = 0u;
    payload.pad1          = 0u;
    payload.model         = g_draw.model;
    payload.inverseModel  = g_draw.inverseModel;

    bool visible = false;
    if (dtid < g_draw.meshletCount)
    {
        uint globalMeshletIdx = g_draw.meshletOffset + dtid;
        MeshletDesc desc = g_meshletDescs[globalMeshletIdx];

        float3 worldCenter = mul(float4(desc.boundsCenter, 1.0f), g_draw.model).xyz;

        float sx = length(g_draw.model[0].xyz);
        float sy = length(g_draw.model[1].xyz);
        float sz = length(g_draw.model[2].xyz);
        float worldRadius = desc.boundsRadius * max(max(sx, sy), sz);

        // Distance-based LOD mirrors Tessellation_HS.hlsl thresholds.
        float dist = length(worldCenter - g_cameraPos);
        uint lod = 0u;
        if      (dist >= 15.0f) lod = 2u;
        else if (dist >=  5.0f) lod = 1u;

        payload.meshletIndex = globalMeshletIdx;
        payload.lodLevel     = lod;

        visible = (lod < 2u) && IsSphereVisible(worldCenter, worldRadius, g_viewProj);
    }

    // D3D12 amplification shaders must call DispatchMesh exactly once on all paths.
    DispatchMesh(visible ? 1u : 0u, 1u, 1u, payload);
}
