#ifndef SASAMI_SWRT_TRAVERSAL_HLSLI
#define SASAMI_SWRT_TRAVERSAL_HLSLI

#include "Shared/Common/RayOffset.hlsli"

// Shared TLAS/BLAS traversal, ray-origin offset, and hit shading geometry helpers.

bool TraceAnyHit(float3 ro, float3 rd, float tMin, float tMax)
{
    float3 rinvd = float3(
        (abs(rd.x) > 1e-7f) ? (1.0f / rd.x) : (rd.x >= 0.0f ? 1e30f : -1e30f),
        (abs(rd.y) > 1e-7f) ? (1.0f / rd.y) : (rd.y >= 0.0f ? 1e30f : -1e30f),
        (abs(rd.z) > 1e-7f) ? (1.0f / rd.z) : (rd.z >= 0.0f ? 1e30f : -1e30f));

    // TLAS traversal
    // Stack depth: TLAS height 竕・log2(instanceCount). 16 supports up to ~32k instances.
    int tlasStack[16];
    int tlasTop = 0;
    tlasStack[tlasTop++] = 0;

    while (tlasTop > 0)
    {
        TlasNode tnode = g_tlasNodes[tlasStack[--tlasTop]];
        float tEntry;
        if (!IntersectRayAabb(ro, rinvd, tnode.bMin, tnode.bMax, tMax, tEntry))
            continue;

        if (tnode.leftChild < 0)
        {
            // Leaf: iterate instances
            uint firstInst = (uint)(~tnode.leftChild);
            uint instCount = (uint)(tnode.rightOrCount);
            for (uint ii = 0; ii < instCount; ++ii)
            {
                GpuInstanceInfo inst = g_instances[firstInst + ii];
                float iEntry;
                if (!IntersectRayAabb(ro, rinvd, inst.worldBoundsMin, inst.worldBoundsMax, tMax, iEntry))
                    continue;

                // Transform ray to local space
                float3 localOrig = TransformPoint4x4(inst.invWorld, ro);
                float3 localDir  = TransformVector4x4(inst.invWorld, rd);
                float  localScale = length(localDir);
                if (localScale < 1e-7f) continue;
                localDir /= localScale;
                float3 localInvD = float3(
                    (abs(localDir.x) > 1e-7f) ? (1.0f / localDir.x) : (localDir.x >= 0.0f ? 1e30f : -1e30f),
                    (abs(localDir.y) > 1e-7f) ? (1.0f / localDir.y) : (localDir.y >= 0.0f ? 1e30f : -1e30f),
                    (abs(localDir.z) > 1e-7f) ? (1.0f / localDir.z) : (localDir.z >= 0.0f ? 1e30f : -1e30f));
                float localTMin = tMin * localScale;
                float localTMax = tMax * localScale;

                GpuMeshInfo mesh = g_meshInfos[inst.meshIndex];

                // BLAS traversal
                // Stack depth: BVH height 竕・log2(triCount / leafSize). 24 supports up to ~100k tris/mesh.
                int blasStack[24];
                int blasTop = 0;
                blasStack[blasTop++] = (int)mesh.nodeOffset;

                while (blasTop > 0)
                {
                    BvhNode bnode = g_bvhNodes[blasStack[--blasTop]];
                    float bEntry;
                    if (!IntersectRayAabb(localOrig, localInvD, bnode.bMin, bnode.bMax, localTMax, bEntry))
                        continue;

                    if (bnode.leftChild < 0)
                    {
                        // Leaf: test triangles
                        uint firstTri = (uint)(~bnode.leftChild);
                        uint triCount = (uint)(bnode.rightOrCount);
                        for (uint ti = 0; ti < triCount; ++ti)
                        {
                            GpuTriangle tri = g_triangles[mesh.triOffset + firstTri + ti];
                            float t, u, v;
                            if (IntersectRayTriangle(localOrig, localDir,
                                                     tri.p0, tri.edge1, tri.edge2,
                                                     t, u, v))
                            {
                                if (t >= localTMin && t < localTMax)
                                    return true;
                            }
                        }
                    }
                    else
                    {
                        if (blasTop + 2 <= 24)
                        {
                            blasStack[blasTop++] = bnode.rightOrCount;
                            blasStack[blasTop++] = bnode.leftChild;
                        }
                    }
                }
            }
        }
        else
        {
            if (tlasTop + 2 <= 16)
            {
                tlasStack[tlasTop++] = tnode.rightOrCount;
                tlasStack[tlasTop++] = tnode.leftChild;
            }
        }
    }
    return false;
}

// Closest-hit result
struct HitResult
{
    bool   hit;
    float  t;
    float  u;
    float  v;
    uint   instanceIndex;
    uint   triLocalIndex; // triangle index within mesh (relative to mesh.triOffset)
};

HitResult TraceClosestHit(float3 ro, float3 rd, float tMin, float tMax)
{
    HitResult result;
    result.hit = false;
    result.t   = tMax;
    result.u   = 0.0f;
    result.v   = 0.0f;
    result.instanceIndex = 0;
    result.triLocalIndex = 0;

    float3 rinvd = float3(
        (abs(rd.x) > 1e-7f) ? (1.0f / rd.x) : (rd.x >= 0.0f ? 1e30f : -1e30f),
        (abs(rd.y) > 1e-7f) ? (1.0f / rd.y) : (rd.y >= 0.0f ? 1e30f : -1e30f),
        (abs(rd.z) > 1e-7f) ? (1.0f / rd.z) : (rd.z >= 0.0f ? 1e30f : -1e30f));

    // Stack depth: TLAS height 竕・log2(instanceCount). 16 supports up to ~32k instances.
    int tlasStack[16];
    int tlasTop = 0;
    tlasStack[tlasTop++] = 0;

    while (tlasTop > 0)
    {
        TlasNode tnode = g_tlasNodes[tlasStack[--tlasTop]];
        float tEntry;
        if (!IntersectRayAabb(ro, rinvd, tnode.bMin, tnode.bMax, result.t, tEntry))
            continue;

        if (tnode.leftChild < 0)
        {
            uint firstInst = (uint)(~tnode.leftChild);
            uint instCount = (uint)(tnode.rightOrCount);
            for (uint ii = 0; ii < instCount; ++ii)
            {
                uint instIdx = firstInst + ii;
                GpuInstanceInfo inst = g_instances[instIdx];
                float iEntry;
                if (!IntersectRayAabb(ro, rinvd, inst.worldBoundsMin, inst.worldBoundsMax, result.t, iEntry))
                    continue;

                float3 localOrig = TransformPoint4x4(inst.invWorld, ro);
                float3 localDir  = TransformVector4x4(inst.invWorld, rd);
                float  localScale = length(localDir);
                if (localScale < 1e-7f) continue;
                localDir /= localScale;
                float3 localInvD = float3(
                    (abs(localDir.x) > 1e-7f) ? (1.0f / localDir.x) : (localDir.x >= 0.0f ? 1e30f : -1e30f),
                    (abs(localDir.y) > 1e-7f) ? (1.0f / localDir.y) : (localDir.y >= 0.0f ? 1e30f : -1e30f),
                    (abs(localDir.z) > 1e-7f) ? (1.0f / localDir.z) : (localDir.z >= 0.0f ? 1e30f : -1e30f));
                float localTMin = tMin * localScale;
                float localTMax = result.t * localScale;

                GpuMeshInfo mesh = g_meshInfos[inst.meshIndex];

                // Stack depth: BVH height 竕・log2(triCount / leafSize). 24 supports up to ~100k tris/mesh.
                int blasStack[24];
                int blasTop = 0;
                blasStack[blasTop++] = (int)mesh.nodeOffset;

                while (blasTop > 0)
                {
                    BvhNode bnode = g_bvhNodes[blasStack[--blasTop]];
                    float bEntry;
                    if (!IntersectRayAabb(localOrig, localInvD, bnode.bMin, bnode.bMax, localTMax, bEntry))
                        continue;

                    if (bnode.leftChild < 0)
                    {
                        uint firstTri = (uint)(~bnode.leftChild);
                        uint triCount = (uint)(bnode.rightOrCount);
                        for (uint ti = 0; ti < triCount; ++ti)
                        {
                            GpuTriangle tri = g_triangles[mesh.triOffset + firstTri + ti];
                            float t, u, v;
                            if (IntersectRayTriangle(localOrig, localDir,
                                                     tri.p0, tri.edge1, tri.edge2,
                                                     t, u, v))
                            {
                                float worldT = t / localScale;
                                if (worldT >= tMin && worldT < result.t)
                                {
                                    result.hit           = true;
                                    result.t             = worldT;
                                    result.u             = u;
                                    result.v             = v;
                                    result.instanceIndex = instIdx;
                                    result.triLocalIndex = firstTri + ti;
                                    localTMax            = t; // update local tMax for future tests
                                }
                            }
                        }
                    }
                    else
                    {
                        if (blasTop + 2 <= 24)
                        {
                            blasStack[blasTop++] = bnode.rightOrCount;
                            blasStack[blasTop++] = bnode.leftChild;
                        }
                    }
                }
            }
        }
        else
        {
            if (tlasTop + 2 <= 16)
            {
                tlasStack[tlasTop++] = tnode.rightOrCount;
                tlasStack[tlasTop++] = tnode.leftChild;
            }
        }
    }
    return result;
}

// OffsetRay() lives in Shared/Common/RayOffset.hlsli.

// Get interpolated world-space normal from a hit
float3 GetWorldNormal(HitResult hit)
{
    GpuInstanceInfo inst = g_instances[hit.instanceIndex];
    GpuMeshInfo     mesh = g_meshInfos[inst.meshIndex];
    GpuTriangle     tri  = g_triangles[mesh.triOffset + hit.triLocalIndex];

    float w = 1.0f - hit.u - hit.v;
    float3 localNormal = normalize(tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v);
    // Correct normal transform: n_world = (M^{-T}) * n_local  (column-vector convention).
    // Because invWorld in the StructuredBuffer is the C++ M^{-1} read as HLSL column-major,
    // HLSL's invWorld == M^{-T} (the transpose of the C++ inverse).
    // Therefore  mul(invWorld, float4(n,0)) == M^{-T} * n 窶・the correct inverse-transpose.
    float3 worldNormal = normalize(mul(inst.invWorld, float4(localNormal, 0.0f)).xyz);
    return worldNormal;
}

#endif // SASAMI_SWRT_TRAVERSAL_HLSLI
