//
// WindParticleStep_CS.hlsl
// GPU twin of StepWindParticle (WindDrivenParticles.cpp): one thread per particle, drag toward the
// steady projected wind, buoyancy/flutter, spin, wall push-out. The SDF and the staggered velocity are
// sampled exactly like ProjectedWindField (SampleSdfTrilinear, StaggeredVelocityGrid::Sample).
// To stay comparable with the CPU every intermediate is `precise` and written one operation per
// statement in the CPU's evaluation order, so the compiler cannot fuse anything into mad/fma.
// sin, sqrt and division may still differ from the CPU by an ulp.
//
// Bindings (one descriptor table per slot, see GpuWindParticleStepper):
//   t0 : g_sdf        signed distance in voxel units, dimX*dimY*dimZ, x fastest
//   t1 : g_u          (dimX+1)*dimY*dimZ
//   t2 : g_v          dimX*(dimY+1)*dimZ
//   t3 : g_w          dimX*dimY*(dimZ+1)
//   t4 : g_params     single StepParams element
//   u0 : g_particles  WindParticle array (64 bytes each)
//

struct WindParticle
{
    float3 position;
    float  age;
    float3 velocity;
    float  lifetime;
    float  angle;
    float  spin;
    float  phase;
    uint   alive;
    float  flutterAxisX;
    float  flutterAxisZ;
    uint   wallHits;
    uint   pad;
};

struct StepParams
{
    float3 origin;
    float  voxelSize;
    float  invVoxelSize;
    uint   dimX;
    uint   dimY;
    uint   dimZ;
    float  dt;
    float  tau;
    float  decay;
    float  buoyancy;
    float3 acceleration;
    float  flutterAcceleration;
    float  flutterFrequency;
    float  wallFriction;
    uint   flutter;
    uint   particleCount;
};

StructuredBuffer<float> g_sdf : register(t0);
StructuredBuffer<float> g_u : register(t1);
StructuredBuffer<float> g_v : register(t2);
StructuredBuffer<float> g_w : register(t3);
StructuredBuffer<StepParams> g_params : register(t4);
RWStructuredBuffer<WindParticle> g_particles : register(u0);

static const float TWO_PI = 6.28318530717958647692f;
static const float PUSH_OUT_VOXELS = 0.05f;

float Fetch(uint which, uint index)
{
    if (which == 0)
    {
        return g_sdf[index];
    }
    if (which == 1)
    {
        return g_u[index];
    }
    if (which == 2)
    {
        return g_v[index];
    }
    return g_w[index];
}

// Trilinear sample of lattice `which` (0 sdf, 1 u, 2 v, 3 w) with latticeDim points per axis;
// (gx, gy, gz) is clamped to [0, latticeDim - 1]. Mirrors SampleLattice / SampleSdfTrilinear.
float SampleLattice(uint which, uint3 latticeDim, float gx, float gy, float gz)
{
    precise float g[3];
    g[0] = gx;
    g[1] = gy;
    g[2] = gz;
    uint i0[3];
    uint i1[3];
    precise float frac[3];
    [unroll]
    for (int axis = 0; axis < 3; ++axis)
    {
        const uint dim = latticeDim[axis];
        precise float maxIndex = (float)(dim - 1u);
        precise float lowClamped = max(g[axis], 0.0f);
        precise float c = min(lowClamped, maxIndex);
        precise float f = floor(c);
        i0[axis] = (uint)f;
        i1[axis] = min(i0[axis] + 1u, dim - 1u);
        frac[axis] = c - f;
    }

    const uint d0 = latticeDim.x;
    const uint d1 = latticeDim.y;
    precise float c000 = Fetch(which, i0[0] + d0 * (i0[1] + d1 * i0[2]));
    precise float c100 = Fetch(which, i1[0] + d0 * (i0[1] + d1 * i0[2]));
    precise float c010 = Fetch(which, i0[0] + d0 * (i1[1] + d1 * i0[2]));
    precise float c110 = Fetch(which, i1[0] + d0 * (i1[1] + d1 * i0[2]));
    precise float c001 = Fetch(which, i0[0] + d0 * (i0[1] + d1 * i1[2]));
    precise float c101 = Fetch(which, i1[0] + d0 * (i0[1] + d1 * i1[2]));
    precise float c011 = Fetch(which, i0[0] + d0 * (i1[1] + d1 * i1[2]));
    precise float c111 = Fetch(which, i1[0] + d0 * (i1[1] + d1 * i1[2]));

    precise float t;
    precise float x00;
    precise float x10;
    precise float x01;
    precise float x11;
    t = c100 - c000;
    t = t * frac[0];
    x00 = c000 + t;
    t = c110 - c010;
    t = t * frac[0];
    x10 = c010 + t;
    t = c101 - c001;
    t = t * frac[0];
    x01 = c001 + t;
    t = c111 - c011;
    t = t * frac[0];
    x11 = c011 + t;

    precise float y0;
    precise float y1;
    t = x10 - x00;
    t = t * frac[1];
    y0 = x00 + t;
    t = x11 - x01;
    t = t * frac[1];
    y1 = x01 + t;

    t = y1 - y0;
    t = t * frac[2];
    precise float result = y0 + t;
    return result;
}

bool Contains(StepParams sp, float3 p)
{
    precise float dims[3];
    dims[0] = (float)sp.dimX;
    dims[1] = (float)sp.dimY;
    dims[2] = (float)sp.dimZ;
    [unroll]
    for (int axis = 0; axis < 3; ++axis)
    {
        precise float low = sp.origin[axis];
        precise float extent = dims[axis] * sp.voxelSize;
        precise float high = low + extent;
        if (p[axis] < low || p[axis] > high)
        {
            return false;
        }
    }
    return true;
}

float Distance(StepParams sp, float3 p)
{
    precise float vx = p.x - sp.origin.x;
    vx = vx * sp.invVoxelSize;
    precise float vy = p.y - sp.origin.y;
    vy = vy * sp.invVoxelSize;
    precise float vz = p.z - sp.origin.z;
    vz = vz * sp.invVoxelSize;
    precise float sx = vx - 0.5f;
    precise float sy = vy - 0.5f;
    precise float sz = vz - 0.5f;
    precise float s = SampleLattice(0, uint3(sp.dimX, sp.dimY, sp.dimZ), sx, sy, sz);
    precise float d = s * sp.voxelSize;
    return d;
}

float3 Velocity(StepParams sp, float3 p)
{
    precise float vx = p.x - sp.origin.x;
    vx = vx * sp.invVoxelSize;
    precise float vy = p.y - sp.origin.y;
    vy = vy * sp.invVoxelSize;
    precise float vz = p.z - sp.origin.z;
    vz = vz * sp.invVoxelSize;
    precise float vxh = vx - 0.5f;
    precise float vyh = vy - 0.5f;
    precise float vzh = vz - 0.5f;
    precise float3 result;
    result.x = SampleLattice(1, uint3(sp.dimX + 1u, sp.dimY, sp.dimZ), vx, vyh, vzh);
    result.y = SampleLattice(2, uint3(sp.dimX, sp.dimY + 1u, sp.dimZ), vxh, vy, vzh);
    result.z = SampleLattice(3, uint3(sp.dimX, sp.dimY, sp.dimZ + 1u), vxh, vyh, vz);
    return result;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const StepParams sp = g_params[0];
    const uint index = dtid.x;
    if (index >= sp.particleCount)
    {
        return;
    }
    WindParticle s = g_particles[index];
    if (s.alive == 0)
    {
        return;
    }

    precise float3 p = s.position;
    precise float3 v = s.velocity;

    precise float3 u = float3(0.0f, 0.0f, 0.0f);
    if (Contains(sp, p))
    {
        u = Velocity(sp, p);
    }

    precise float ratio = s.age / s.lifetime;
    precise float ratioLow = max(ratio, 0.0f);
    precise float age01 = min(ratioLow, 1.0f);
    precise float oneMinusAge = 1.0f - age01;
    precise float buoy = sp.buoyancy * oneMinusAge;
    precise float3 a;
    a.x = sp.acceleration.x;
    a.y = sp.acceleration.y + buoy;
    a.z = sp.acceleration.z;
    if (sp.flutter != 0)
    {
        precise float phaseRate = TWO_PI * sp.flutterFrequency;
        phaseRate = phaseRate * sp.dt;
        s.phase = s.phase + phaseRate;
        precise float sinPhase = sin(s.phase);
        precise float side = sp.flutterAcceleration * sinPhase;
        precise float sx = s.flutterAxisX * side;
        precise float sy = 0.0f * side;
        precise float sz = s.flutterAxisZ * side;
        a.x = a.x + sx;
        a.y = a.y + sy;
        a.z = a.z + sz;
    }
    precise float spinStep = s.spin * sp.dt;
    s.angle = s.angle + spinStep;

    precise float oneMinusDecay = 1.0f - sp.decay;
    [unroll]
    for (int c = 0; c < 3; ++c)
    {
        precise float aTau = a[c] * sp.tau;
        precise float target = u[c] + aTau;
        precise float offset = v[c] - target;
        precise float t1 = target * sp.dt;
        precise float t2 = offset * sp.tau;
        t2 = t2 * oneMinusDecay;
        precise float t3 = t1 + t2;
        p[c] = p[c] + t3;
        precise float t4 = offset * sp.decay;
        v[c] = target + t4;
    }
    s.age = s.age + sp.dt;

    precise float d = Distance(sp, p);
    if (d < 0.0f)
    {
        s.wallHits = s.wallHits + 1u;
        precise float h = 0.5f * sp.voxelSize;
        precise float3 n;
        [unroll]
        for (int k = 0; k < 3; ++k)
        {
            precise float3 hi = p;
            precise float3 lo = p;
            hi[k] = hi[k] + h;
            lo[k] = lo[k] - h;
            precise float dHi = Distance(sp, hi);
            precise float dLo = Distance(sp, lo);
            n[k] = dHi - dLo;
        }
        precise float nx2 = n.x * n.x;
        precise float ny2 = n.y * n.y;
        precise float nz2 = n.z * n.z;
        precise float sum = nx2 + ny2;
        sum = sum + nz2;
        precise float len = sqrt(sum);
        if (len < 1e-6f)
        {
            n = float3(0.0f, 1.0f, 0.0f);
        }
        else
        {
            n.x = n.x / len;
            n.y = n.y / len;
            n.z = n.z / len;
        }
        precise float negD = -d;
        precise float pushBias = PUSH_OUT_VOXELS * sp.voxelSize;
        precise float push = negD + pushBias;
        [unroll]
        for (int m = 0; m < 3; ++m)
        {
            precise float move = n[m] * push;
            p[m] = p[m] + move;
        }
        precise float vnx = v.x * n.x;
        precise float vny = v.y * n.y;
        precise float vnz = v.z * n.z;
        precise float vn = vnx + vny;
        vn = vn + vnz;
        if (vn < 0.0f)
        {
            [unroll]
            for (int q = 0; q < 3; ++q)
            {
                precise float remove = vn * n[q];
                v[q] = v[q] - remove;
            }
        }
        [unroll]
        for (int r = 0; r < 3; ++r)
        {
            v[r] = v[r] * sp.wallFriction;
        }
    }

    if (s.age >= s.lifetime)
    {
        s.alive = 0;
    }
    else if (!Contains(sp, p))
    {
        s.alive = 0;
    }

    s.position = p;
    s.velocity = v;
    g_particles[index] = s;
}
