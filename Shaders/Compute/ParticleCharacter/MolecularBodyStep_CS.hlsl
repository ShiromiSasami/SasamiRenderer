//
// MolecularBodyStep_CS.hlsl
// GPU twin of MolecularBody::StepRange (MolecularBody.cpp): one thread per particle. Bond update,
// release (+ scatter kick), free flight (gravity or swirl, drag), Holden's exact spring toward the
// target, floor. To stay comparable with the CPU every intermediate is `precise` and written one
// operation per statement in the CPU's evaluation order, so the compiler cannot fuse anything into
// mad/fma. exp, sqrt and division may still differ from the CPU by an ulp.
//
// Bindings (inline root descriptors, see GpuMolecularBodyStepper; t0..t2 are offset per step):
//   t0 : g_targets    count * 3 floats of this step
//   t1 : g_normals    count * 3 floats of this step
//   t2 : g_params     single StepParams element of this step
//   u0 : g_particles  MolecularParticle array (48 bytes each)
// DXIL, so DX12 only for now.
//

struct StepParams
{
    float  halflife;
    float  bondRate;
    float  bondDelaySpread;
    float  bondDelayPerHeight;
    uint   force;
    float  scatterSpeed;
    float  swirlRate;
    float  swirlRise;
    float  gravity;
    float  drag;
    float  floorY;
    float  restitution;
    float3 centre;
    float  dt;
    float  bondGoal;
    float  sinceGoalChange;
    uint   count;
    float  fourLn2;
    uint   resetReleased;
    uint   pad0;
    uint   pad1;
    uint   pad2;
    uint   pad3;
    uint   pad4;
    uint   pad5;
    uint   pad6;
};

struct MolecularParticle
{
    float3 position;
    float  bond;
    float3 velocity;
    float  delayHash;
    uint   released;
    uint   floorHits;
    uint2  pad;
};

StructuredBuffer<float> g_targets : register(t0);
StructuredBuffer<float> g_normals : register(t1);
StructuredBuffer<StepParams> g_params : register(t2);
RWStructuredBuffer<MolecularParticle> g_particles : register(u0);

// Anim::SimpleSpringDamper for one component.
void SpringComponent(inout float x, inout float v, float goal, float halflife, float fourLn2, float dt)
{
    precise float hEps = halflife + 1e-5f;
    precise float damping = fourLn2 / hEps;
    precise float y = damping / 2.0f;
    precise float j0 = x - goal;
    precise float j0y = j0 * y;
    precise float j1 = v + j0y;
    precise float yDt = y * dt;
    precise float negYDt = -yDt;
    precise float e = exp(negYDt);
    precise float j1Dt = j1 * dt;
    precise float sum = j0 + j1Dt;
    precise float eSum = e * sum;
    precise float j1y = j1 * y;
    precise float j1yDt = j1y * dt;
    precise float vDiff = v - j1yDt;
    precise float newV = e * vDiff;
    precise float newX = eSum + goal;
    x = newX;
    v = newV;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const StepParams sp = g_params[0];
    const uint i = dtid.x;
    if (i >= sp.count)
    {
        return;
    }
    MolecularParticle s = g_particles[i];
    if (sp.resetReleased != 0)
    {
        s.released = 0;
    }

    precise float3 x = s.position;
    precise float3 v = s.velocity;
    precise float3 t;
    t.x = g_targets[i * 3u + 0u];
    t.y = g_targets[i * 3u + 1u];
    t.z = g_targets[i * 3u + 2u];
    precise float3 n;
    n.x = g_normals[i * 3u + 0u];
    n.y = g_normals[i * 3u + 1u];
    n.z = g_normals[i * 3u + 2u];

    precise float delay = s.delayHash * sp.bondDelaySpread;
    precise float tHeight = t.y - sp.centre.y;
    precise float delayHeight = sp.bondDelayPerHeight * tHeight;
    delay = delay + delayHeight;
    delay = max(delay, 0.0f);
    const bool active = sp.sinceGoalChange >= delay;

    precise float b = s.bond;
    if (active)
    {
        precise float maxChange = sp.bondRate * sp.dt;
        precise float diff = sp.bondGoal - b;
        precise float absDiff = abs(diff);
        if (absDiff <= maxChange)
        {
            b = sp.bondGoal;
        }
        else if (diff > 0.0f)
        {
            b = b + maxChange;
        }
        else
        {
            b = b - maxChange;
        }
    }

    if (sp.bondGoal < 0.5f && s.released == 0 && active)
    {
        s.released = 1;
        if (sp.force == 0)
        {
            precise float kx = n.x * sp.scatterSpeed;
            precise float ky = n.y * sp.scatterSpeed;
            precise float kz = n.z * sp.scatterSpeed;
            v.x = v.x + kx;
            v.y = v.y + ky;
            v.z = v.z + kz;
        }
    }

    precise float free = 1.0f - b;
    if (free > 0.0f)
    {
        precise float3 a;
        a.x = 0.0f;
        a.y = -sp.gravity;
        a.z = 0.0f;
        if (sp.force == 1)
        {
            precise float rx = x.x - sp.centre.x;
            precise float rz = x.z - sp.centre.z;
            precise float rx2 = rx * rx;
            precise float rz2 = rz * rz;
            precise float r2 = rx2 + rz2;
            precise float len = sqrt(r2);
            precise float3 tangent = float3(0.0f, 0.0f, 0.0f);
            if (len > 1e-6f)
            {
                precise float negRz = -rz;
                tangent.x = negRz / len;
                tangent.y = 0.0f;
                tangent.z = rx / len;
            }
            precise float rate = sp.swirlRate * len;
            precise float speed = min(rate, 6.0f);
            precise float3 desired;
            precise float dx = tangent.x * speed;
            precise float dy = tangent.y * speed;
            precise float dz = tangent.z * speed;
            desired.x = dx + 0.0f;
            desired.y = dy + sp.swirlRise;
            desired.z = dz + 0.0f;
            precise float ex = desired.x - v.x;
            precise float ey = desired.y - v.y;
            precise float ez = desired.z - v.z;
            a.x = ex * 4.0f;
            a.y = ey * 4.0f;
            a.z = ez * 4.0f;
        }
        precise float dtFree = sp.dt * free;
        precise float ax = a.x * dtFree;
        precise float ay = a.y * dtFree;
        precise float az = a.z * dtFree;
        v.x = v.x + ax;
        v.y = v.y + ay;
        v.z = v.z + az;
        precise float negDrag = -sp.drag;
        precise float dragDt = negDrag * sp.dt;
        precise float dragFree = dragDt * free;
        precise float decay = exp(dragFree);
        v.x = v.x * decay;
        v.y = v.y * decay;
        v.z = v.z * decay;
        precise float mx = v.x * dtFree;
        precise float my = v.y * dtFree;
        precise float mz = v.z * dtFree;
        x.x = x.x + mx;
        x.y = x.y + my;
        x.z = x.z + mz;
    }

    if (b > 1e-4f)
    {
        precise float h = sp.halflife / b;
        SpringComponent(x.x, v.x, t.x, h, sp.fourLn2, sp.dt);
        SpringComponent(x.y, v.y, t.y, h, sp.fourLn2, sp.dt);
        SpringComponent(x.z, v.z, t.z, h, sp.fourLn2, sp.dt);
    }

    if (x.y < sp.floorY)
    {
        x.y = sp.floorY;
        if (v.y < 0.0f)
        {
            precise float negVy = -v.y;
            v.y = negVy * sp.restitution;
        }
        s.floorHits = s.floorHits + 1u;
    }

    s.bond = b;
    s.position = x;
    s.velocity = v;
    g_particles[i] = s;
}
