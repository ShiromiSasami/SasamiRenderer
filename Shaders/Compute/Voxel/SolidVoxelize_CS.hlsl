//
// SolidVoxelize_CS.hlsl
// Triangle-parallel solid voxelization by parity into a dense 1-bit occupancy
// grid (Schwarz & Seidel, "Fast Parallel Surface and Solid Voxelization on
// GPUs", SIGGRAPH Asia 2010, sec. 4). Pass 1 (CSCrossings) gives each thread
// one triangle: for every voxel row (y, z) whose centre the triangle covers in
// the YZ plane, it XORs a single bit at the first voxel past the crossing
// along +X. Pass 2 (CSPrefix) gives each thread one row and turns those single
// bits into the flipped row tails with a prefix XOR.
//
// This is a line-for-line port of the CPU reference
// (Source/Renderer/Voxel/CpuSolidVoxelizer.cpp); GpuSolidVoxelizer's self-test
// compares the two bit for bit. To keep that comparison exact:
//   - the coverage and crossing tests run in double, using only add, subtract,
//     multiply, compare, negate, select and float->double conversion. There is
//     no division, no mad/fma, no floor/abs intrinsic and no int<->double
//     conversion (an extended double op); integers reach double through float
//     as (double)(float)n, which is exact for any realistic grid index;
//   - every double expression that multiplies and then adds or subtracts is
//     stored in a `precise` variable, so DXC does not fuse it into an fma the
//     CPU never performs, and the CPU's evaluation order is kept;
//   - the row range is computed in float and widened by one voxel on each
//     side. It only has to be a superset of the covered rows, because the
//     exact coverage test rejects the extra ones.
//
// Grid layout is row-padded, unlike OccupancyBitGrid: row r = y + z*dim.y owns
// rowWords = ceil(dim.x / 32) words, bit x of row r is word r*rowWords + (x>>5),
// bit x&31. The prefix XOR therefore never crosses a row; the host repacks into
// the dense layout on readback.
//
// Bindings (one descriptor table per slot, see GpuSolidVoxelizer):
//   t0 : g_positions  float stream, positionStride floats per vertex
//   t1 : g_indices    3 per triangle
//   t2 : g_params     single SolidVoxelizeParams element
//   u0 : g_bits       row-padded bit grid, cleared to zero by the host
//

struct SolidVoxelizeParams
{
    float3 origin;
    float  voxelSize;
    uint3  dim;
    uint   triangleCount;
    uint   vertexCount;
    uint   positionStride;
    uint   rowWords;
    uint   rowCount;
    uint   crossingsThreadsPerRow;   // threads along X of the CSCrossings dispatch (groupsX * 64)
    uint   prefixThreadsPerRow;      // threads along X of the CSPrefix dispatch (groupsX * 64)
    uint   pad0;
    uint   pad1;
};

StructuredBuffer<float>               g_positions : register(t0);
StructuredBuffer<uint>                g_indices   : register(t1);
StructuredBuffer<SolidVoxelizeParams> g_params    : register(t2);
RWStructuredBuffer<uint>              g_bits      : register(u0);

float3 LoadPosition(uint vertexIndex, uint stride)
{
    uint base = vertexIndex * stride;
    return float3(g_positions[base], g_positions[base + 1], g_positions[base + 2]);
}

// Edge function in the YZ plane with endpoints in canonical (lexicographic) order, so the
// same double expression results no matter which triangle evaluates a shared edge.
double CanonicalEdgeFunction(double ay, double az, double by, double bz, double py, double pz)
{
    bool aFirst = ay < by || (ay == by && az < bz);
    double py0 = aFirst ? ay : by;
    double pz0 = aFirst ? az : bz;
    double qy0 = aFirst ? by : ay;
    double qz0 = aFirst ? bz : az;
    precise double m0 = (qy0 - py0) * (pz - pz0);
    precise double m1 = (qz0 - pz0) * (py - py0);
    precise double r = m0 - m1;
    return r;
}

// Positive when the centre of voxel x lies at larger x than the crossing, scaled by |nx| so
// that no division is needed. b = ny * (yc - v0y) + nz * (zc - v0z).
bool CentrePastCrossing(double nx, double b, double v0x, double originX, double voxelSize, int x)
{
    double xd = double(float(x));
    precise double cxStep = (xd + 0.5L) * voxelSize;
    precise double cx = originX + cxStep;
    double absNx = nx > 0.0L ? nx : -nx;
    double signedB = nx > 0.0L ? b : -b;
    precise double scaled = absNx * (cx - v0x);
    precise double total = scaled + signedB;
    return total > 0.0L;
}

// Smallest x in [0, dimX) whose centre lies past the crossing, or dimX when none does.
int FirstVoxelPastCrossing(double nx, double b, double v0x, double originX, double voxelSize, int dimX)
{
    int lo = 0;
    int hi = dimX;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (CentrePastCrossing(nx, b, v0x, originX, voxelSize, mid))
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1;
        }
    }
    return lo;
}

[numthreads(64, 1, 1)]
void CSCrossings(uint3 id : SV_DispatchThreadID)
{
    SolidVoxelizeParams p = g_params[0];
    uint tri = id.x + id.y * p.crossingsThreadsPerRow;
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

    float3 f0 = LoadPosition(i0, p.positionStride);
    float3 f1 = LoadPosition(i1, p.positionStride);
    float3 f2 = LoadPosition(i2, p.positionStride);

    double v0x = f0.x;
    double v0y = f0.y;
    double v0z = f0.z;
    double v1x = f1.x;
    double v1y = f1.y;
    double v1z = f1.z;
    double v2x = f2.x;
    double v2y = f2.y;
    double v2z = f2.z;

    precise double dA = (v1y - v0y) * (v2z - v0z);
    precise double dB = (v1z - v0z) * (v2y - v0y);
    precise double d = dA - dB;
    if (d == 0.0L)
    {
        return;
    }

    float minY = min(f0.y, min(f1.y, f2.y));
    float maxY = max(f0.y, max(f1.y, f2.y));
    float minZ = min(f0.z, min(f1.z, f2.z));
    float maxZ = max(f0.z, max(f1.z, f2.z));
    // CPU: floor(...) -/+ 1 clamped to the grid; computed in float here, see header. Clamp in
    // float first so huge coordinates cannot overflow the int conversion.
    float dimYF = float(p.dim.y);
    float dimZF = float(p.dim.z);
    float jLoF = clamp(floor((minY - p.origin.y) / p.voxelSize - 0.5f) - 1.0f, -1.0f, dimYF);
    float jHiF = clamp(floor((maxY - p.origin.y) / p.voxelSize - 0.5f) + 1.0f, -1.0f, dimYF);
    float kLoF = clamp(floor((minZ - p.origin.z) / p.voxelSize - 0.5f) - 1.0f, -1.0f, dimZF);
    float kHiF = clamp(floor((maxZ - p.origin.z) / p.voxelSize - 0.5f) + 1.0f, -1.0f, dimZF);
    int jLo = max(int(jLoF), 0);
    int jHi = min(int(jHiF), int(p.dim.y) - 1);
    int kLo = max(int(kLoF), 0);
    int kHi = min(int(kHiF), int(p.dim.z) - 1);
    if (jLo > jHi || kLo > kHi)
    {
        return;
    }

    // Edge i is (v[i], v[(i+1)%3]) with opposite vertex v[(i+2)%3].
    double edgeSign0 = CanonicalEdgeFunction(v0y, v0z, v1y, v1z, v2y, v2z) > 0.0L ? 1.0L : -1.0L;
    double edgeSign1 = CanonicalEdgeFunction(v1y, v1z, v2y, v2z, v0y, v0z) > 0.0L ? 1.0L : -1.0L;
    double edgeSign2 = CanonicalEdgeFunction(v2y, v2z, v0y, v0z, v1y, v1z) > 0.0L ? 1.0L : -1.0L;

    double nx = d;
    precise double nyA = (v1z - v0z) * (v2x - v0x);
    precise double nyB = (v1x - v0x) * (v2z - v0z);
    precise double ny = nyA - nyB;
    precise double nzA = (v1x - v0x) * (v2y - v0y);
    precise double nzB = (v1y - v0y) * (v2x - v0x);
    precise double nz = nzA - nzB;

    double originX = p.origin.x;
    double originY = p.origin.y;
    double originZ = p.origin.z;
    double voxelSize = p.voxelSize;
    int dimX = int(p.dim.x);

    // Schwarz & Seidel 2010, sec. 4: flip every voxel behind each crossing along +X.
    for (int k = kLo; k <= kHi; ++k)
    {
        double kd = double(float(k));
        precise double zcStep = (kd + 0.5L) * voxelSize;
        precise double zc = originZ + zcStep;
        for (int j = jLo; j <= jHi; ++j)
        {
            double jd = double(float(j));
            precise double ycStep = (jd + 0.5L) * voxelSize;
            precise double yc = originY + ycStep;

            precise double e0 = CanonicalEdgeFunction(v0y, v0z, v1y, v1z, yc, zc);
            precise double e1 = CanonicalEdgeFunction(v1y, v1z, v2y, v2z, yc, zc);
            precise double e2 = CanonicalEdgeFunction(v2y, v2z, v0y, v0z, yc, zc);
            precise double s0 = e0 * edgeSign0;
            precise double s1 = e1 * edgeSign1;
            precise double s2 = e2 * edgeSign2;
            bool covered0 = s0 > 0.0L || (e0 == 0.0L && edgeSign0 > 0.0L);
            bool covered1 = s1 > 0.0L || (e1 == 0.0L && edgeSign1 > 0.0L);
            bool covered2 = s2 > 0.0L || (e2 == 0.0L && edgeSign2 > 0.0L);
            if (!(covered0 && covered1 && covered2))
            {
                continue;
            }

            precise double bY = ny * (yc - v0y);
            precise double bZ = nz * (zc - v0z);
            precise double b = bY + bZ;

            int q = FirstVoxelPastCrossing(nx, b, v0x, originX, voxelSize, dimX);
            if (q >= dimX)
            {
                continue;
            }

            uint row = uint(j) + uint(k) * p.dim.y;
            InterlockedXor(g_bits[row * p.rowWords + (uint(q) >> 5)], 1u << (uint(q) & 31u));
        }
    }
}

[numthreads(64, 1, 1)]
void CSPrefix(uint3 id : SV_DispatchThreadID)
{
    SolidVoxelizeParams p = g_params[0];
    uint row = id.x + id.y * p.prefixThreadsPerRow;
    if (row >= p.rowCount)
    {
        return;
    }

    // Each crossing bit is the first voxel past a crossing; the prefix XOR (inclusive scan
    // along +X) sets that voxel and every later one in the row, i.e. flips the row tail the CPU
    // reference writes. Within a word the shifts form a log-step scan; `carry` is the parity of
    // all earlier words and inverts the word when odd.
    uint carry = 0;
    for (uint w = 0; w < p.rowWords; ++w)
    {
        uint index = row * p.rowWords + w;
        uint v = g_bits[index];
        v ^= v << 1;
        v ^= v << 2;
        v ^= v << 4;
        v ^= v << 8;
        v ^= v << 16;
        if (carry != 0)
        {
            v = ~v;
        }
        g_bits[index] = v;
        carry = v >> 31;
    }
}
