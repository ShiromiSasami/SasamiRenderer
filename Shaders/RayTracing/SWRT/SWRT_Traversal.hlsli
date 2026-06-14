#ifndef SASAMI_SWRT_TRAVERSAL_HLSLI
#define SASAMI_SWRT_TRAVERSAL_HLSLI

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

// --------------------------------------------------------------------------
// NVIDIA Error-Bounded Ray Origin Offset
// Source: "Solving Self-Intersection Artifacts in DirectX Raytracing"
//         https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/
//
// Offsets ray origin p along geometric normal n by an amount proportional to
// the floating-point ULP of each coordinate.  This guarantees the spawned ray
// never re-intersects the surface it was launched from, even for geometry that
// is far from the world origin (where a fixed bias would be too small) or very
// close to it (where manipulating ULPs alone would under-offset).
//
// Usage:  float3 safeOrigin = OffsetRay(hitPos, geometricNormal);
//         TraceClosestHit(safeOrigin, reflDir, 0.0f, tMax);
// --------------------------------------------------------------------------
float3 OffsetRay(float3 p, float3 n)
{
    static const float kOrigin     = 1.0f / 32.0f;   // linear region threshold
    static const float kFloatScale = 1.0f / 65536.0f; // linear region scale
    static const float kIntScale   = 256.0f;           // ULP nudge scale

    // Compute per-axis ULP offset as integer bit-manipulation
    int3 of_i = int3(kIntScale * n.x,
                     kIntScale * n.y,
                     kIntScale * n.z);

    // Add/subtract ULPs depending on sign of p (offset always away from surface)
    float3 p_i = float3(
        asfloat(asint(p.x) + (p.x < 0.0f ? -of_i.x : of_i.x)),
        asfloat(asint(p.y) + (p.y < 0.0f ? -of_i.y : of_i.y)),
        asfloat(asint(p.z) + (p.z < 0.0f ? -of_i.z : of_i.z)));

    // Near origin: ULP is too small, fall back to a linear offset
    return float3(
        abs(p.x) < kOrigin ? p.x + kFloatScale * n.x : p_i.x,
        abs(p.y) < kOrigin ? p.y + kFloatScale * n.y : p_i.y,
        abs(p.z) < kOrigin ? p.z + kFloatScale * n.z : p_i.z);
}

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
