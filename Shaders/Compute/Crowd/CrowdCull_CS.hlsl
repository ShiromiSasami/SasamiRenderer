//
// CrowdCull_CS.hlsl
// GPU frustum culling + distance LOD selection for the crowd. CSReset (1 thread) writes one
// D3D12_DRAW_INDEXED_ARGUMENTS record per LOD {indexCount, 0, firstIndex, 0, 0}; CSCull (one
// thread per agent) tests the agent's bounding sphere against the 6 frustum planes
// (Gribb & Hartmann 2001, "Fast Extraction of Viewing Frustum Planes"), picks the LOD from the
// camera distance, bumps that LOD's InstanceCount and appends the agent index to the LOD's
// compacted list. Pattern after Wihlidal, "Optimizing the Graphics Pipeline with Compute"
// (GDC 2016).
//
// Bindings (inline root descriptors, see CrowdGpuCuller):
//   t0 : g_params   single CullParams element
//   u0 : g_agents   CrowdAgent array (read only)
//   u1 : g_args     raw, CROWD_MAX_LODS * 20 bytes of DrawIndexed arguments
//   u2 : g_visible  CROWD_MAX_LODS * capacity agent indices, LOD i starts at i * capacity
//

#define CROWD_MAX_LODS 3

struct CrowdAgent
{
    float posX;
    float posY;
    float posZ;
    float animTime;
    float velX;
    float velZ;
    float animRate;
    uint  clip;
};

struct CullParams
{
    float4 planes[6];       // xyz normal, w distance; inside when dot(n,c)+w >= -r
    float3 cameraPos;
    uint   agentCount;
    float  sphereCenterY;
    float  sphereRadius;
    float  modelScale;
    float  modelYOffset;
    float  lodDistance1;
    float  lodDistance2;
    uint   lodCount;
    int    forceLod;
    uint   capacity;
    uint   frustumEnabled;
    uint   threadsPerRow;   // threads along X of the dispatch (groupsX * 64)
    uint   pad0;
    uint4  lodIndexCount;
    uint4  lodFirstIndex;
};

StructuredBuffer<CullParams> g_params : register(t0);
RWStructuredBuffer<CrowdAgent> g_agents : register(u0);
RWByteAddressBuffer g_args : register(u1);
RWStructuredBuffer<uint> g_visible : register(u2);

[numthreads(1, 1, 1)]
void CSReset()
{
    CullParams p = g_params[0];
    for (uint lod = 0; lod < CROWD_MAX_LODS; ++lod)
    {
        uint offset = lod * 20;
        if (lod < p.lodCount)
        {
            g_args.Store(offset + 0, p.lodIndexCount[lod]);
            g_args.Store(offset + 4, 0);
            g_args.Store(offset + 8, p.lodFirstIndex[lod]);
            g_args.Store(offset + 12, 0);
            g_args.Store(offset + 16, 0);
        }
        else
        {
            g_args.Store(offset + 0, 0);
            g_args.Store(offset + 4, 0);
            g_args.Store(offset + 8, 0);
            g_args.Store(offset + 12, 0);
            g_args.Store(offset + 16, 0);
        }
    }
}

[numthreads(64, 1, 1)]
void CSCull(uint3 id : SV_DispatchThreadID)
{
    CullParams p = g_params[0];
    uint index = id.y * p.threadsPerRow + id.x;
    if (index >= p.agentCount)
    {
        return;
    }

    CrowdAgent a = g_agents[index];
    float3 center = float3(a.posX, a.posY + p.sphereCenterY * p.modelScale + p.modelYOffset, a.posZ);
    float radius = p.sphereRadius * p.modelScale;

    if (p.frustumEnabled != 0)
    {
        [unroll]
        for (uint i = 0; i < 6; ++i)
        {
            if (dot(p.planes[i].xyz, center) + p.planes[i].w < -radius)
            {
                return;
            }
        }
    }

    float dist = length(center - p.cameraPos);
    int lod = (dist >= p.lodDistance2) ? 2 : ((dist >= p.lodDistance1) ? 1 : 0);
    if (p.forceLod >= 0)
    {
        lod = p.forceLod;
    }
    lod = min(lod, (int)p.lodCount - 1);

    uint slot;
    g_args.InterlockedAdd((uint)lod * 20 + 4, 1, slot);
    if (slot < p.capacity)
    {
        g_visible[(uint)lod * p.capacity + slot] = index;
    }
}
