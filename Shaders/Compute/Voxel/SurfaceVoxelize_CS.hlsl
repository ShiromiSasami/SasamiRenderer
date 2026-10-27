//
// SurfaceVoxelize_CS.hlsl
// Triangle-parallel conservative surface voxelization into a dense 1-bit
// occupancy grid (Schwarz & Seidel, "Fast Parallel Surface and Solid
// Voxelization on GPUs", SIGGRAPH Asia 2010, sec. 3). Each thread owns one
// triangle and tests every voxel of its clamped AABB with the 13-axis
// separating-axis test of Akenine-Moller, "Fast 3D Triangle-Box Overlap
// Testing" (2001).
//
// This is a line-for-line port of the CPU reference
// (Source/Renderer/Voxel/CpuSurfaceVoxelizer.cpp + TriangleBoxOverlap.cpp);
// GpuSurfaceVoxelizer's self-test compares the two bit for bit. To keep that
// comparison exact:
//   - every SAT expression is `precise`, so DXC does not fuse a*b+c into a
//     mad the CPU never performs;
//   - the AABB range is widened by one voxel on each side. The CPU computes
//     it in double; widening only adds voxels that the SAT then rejects, so
//     the result does not depend on float vs double rounding there.
//
// Grid layout matches OccupancyBitGrid: linear = x + y*dimX + z*dimX*dimY,
// word = linear >> 5, bit = linear & 31. Only the region [regionMin,
// regionMin + regionDim) is written, indexed relative to regionMin (the full
// grid is regionMin = 0, regionDim = dim).
//
// Bindings (one descriptor table per slot, see GpuSurfaceVoxelizer):
//   t0 : g_positions  float stream, positionStride floats per vertex
//   t1 : g_indices    3 per triangle
//   t2 : g_params     single VoxelizeParams element
//   u0 : g_bits       dense bit grid, cleared to zero by the host
//

struct VoxelizeParams
{
    float3 origin;
    float  voxelSize;
    uint3  dim;
    uint   triangleCount;
    uint   vertexCount;
    uint   positionStride;
    uint   threadsPerRow;   // threads along X of the dispatch (groupsX * 64)
    uint   pad;
    uint3  regionMin;       // first voxel of the output region, in full-grid indices
    uint   pad1;
    uint3  regionDim;       // output region extent; bits are indexed region-locally
    uint   pad2;
};

StructuredBuffer<float>          g_positions : register(t0);
StructuredBuffer<uint>           g_indices   : register(t1);
StructuredBuffer<VoxelizeParams> g_params    : register(t2);
RWStructuredBuffer<uint>         g_bits      : register(u0);

bool SeparatedOnAxis(float3 axis, float3 h, float3 p0, float3 p1, float3 p2)
{
    precise float d0 = axis.x * p0.x + axis.y * p0.y + axis.z * p0.z;
    precise float d1 = axis.x * p1.x + axis.y * p1.y + axis.z * p1.z;
    precise float d2 = axis.x * p2.x + axis.y * p2.y + axis.z * p2.z;
    float minD = min(d0, min(d1, d2));
    float maxD = max(d0, max(d1, d2));
    precise float r = h.x * abs(axis.x) + h.y * abs(axis.y) + h.z * abs(axis.z);
    return minD > r || maxD < -r;
}

bool TriangleOverlapsBox(float3 boxCenter, float3 h, float3 v0, float3 v1, float3 v2)
{
    precise float3 p0 = v0 - boxCenter;
    precise float3 p1 = v1 - boxCenter;
    precise float3 p2 = v2 - boxCenter;

    precise float3 e0 = p1 - p0;
    precise float3 e1 = p2 - p1;
    precise float3 e2 = p0 - p2;

    [unroll]
    for (int e = 0; e < 3; ++e)
    {
        float3 edge = e == 0 ? e0 : (e == 1 ? e1 : e2);
        if (SeparatedOnAxis(float3(0.0f, -edge.z, edge.y), h, p0, p1, p2)) return false;
        if (SeparatedOnAxis(float3(edge.z, 0.0f, -edge.x), h, p0, p1, p2)) return false;
        if (SeparatedOnAxis(float3(-edge.y, edge.x, 0.0f), h, p0, p1, p2)) return false;
    }

    float3 minV = min(p0, min(p1, p2));
    float3 maxV = max(p0, max(p1, p2));
    if (any(minV > h) || any(maxV < -h))
    {
        return false;
    }

    precise float3 n = float3(e0.y * e1.z - e0.z * e1.y,
                              e0.z * e1.x - e0.x * e1.z,
                              e0.x * e1.y - e0.y * e1.x);
    if (n.x != 0.0f || n.y != 0.0f || n.z != 0.0f)
    {
        precise float r = h.x * abs(n.x) + h.y * abs(n.y) + h.z * abs(n.z);
        precise float d = n.x * p0.x + n.y * p0.y + n.z * p0.z;
        if (abs(d) > r)
        {
            return false;
        }
    }
    return true;
}

float3 LoadPosition(uint vertexIndex, uint stride)
{
    uint base = vertexIndex * stride;
    return float3(g_positions[base], g_positions[base + 1], g_positions[base + 2]);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    VoxelizeParams p = g_params[0];
    uint tri = id.x + id.y * p.threadsPerRow;
    if (tri >= p.triangleCount)
    {
        return;
    }

    uint i0 = g_indices[tri * 3 + 0];
    uint i1 = g_indices[tri * 3 + 1];
    uint i2 = g_indices[tri * 3 + 2];
    if (i0 >= p.vertexCount || i1 >= p.vertexCount || i2 >= p.vertexCount)
    {
        return;
    }

    float3 v0 = LoadPosition(i0, p.positionStride);
    float3 v1 = LoadPosition(i1, p.positionStride);
    float3 v2 = LoadPosition(i2, p.positionStride);

    float3 tMin = (min(v0, min(v1, v2)) - p.origin) / p.voxelSize;
    float3 tMax = (max(v0, max(v1, v2)) - p.origin) / p.voxelSize;
    // CPU: [ceil(tMin) - 1, floor(tMax)]; one extra voxel each side, see header.
    // Clamp in float first so huge coordinates cannot overflow the int conversion.
    float3 maxIndex = float3(p.dim) - 1.0f;
    float3 loF = clamp(floor(tMin) - 1.0f, -1.0f, maxIndex + 1.0f);
    float3 hiF = clamp(floor(tMax) + 1.0f, -1.0f, maxIndex + 1.0f);
    int3 lo = max(int3(loF), int3(0, 0, 0));
    int3 hi = min(int3(hiF), int3(p.dim) - 1);
    lo = max(lo, int3(p.regionMin));
    hi = min(hi, int3(p.regionMin + p.regionDim) - 1);
    if (any(lo > hi))
    {
        return;
    }

    float h = 0.5f * p.voxelSize;
    float3 halfSize = float3(h, h, h);
    for (int z = lo.z; z <= hi.z; ++z)
    {
        for (int y = lo.y; y <= hi.y; ++y)
        {
            for (int x = lo.x; x <= hi.x; ++x)
            {
                precise float3 center = p.origin + (float3(x, y, z) + 0.5f) * p.voxelSize;
                if (TriangleOverlapsBox(center, halfSize, v0, v1, v2))
                {
                    uint3 local = uint3(x, y, z) - p.regionMin;
                    uint bitIndex = local.x + local.y * p.regionDim.x + local.z * p.regionDim.x * p.regionDim.y;
                    InterlockedOr(g_bits[bitIndex >> 5], 1u << (bitIndex & 31u));
                }
            }
        }
    }
}
