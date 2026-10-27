//
// CrowdUpdate_CS.hlsl
// One thread per agent: advances the crowd by one params.deltaTime step. This is an
// operation-for-operation port of the CPU reference StepCrowdRange
// (Source/Renderer/Crowd/CrowdSimulation.h); GpuCrowdUpdater's validation compares the two
// bit for bit. To keep that exact, every intermediate is `precise` and written one operation
// per statement, so DXC does not fuse a*b+c into a mad the CPU never performs.
//
// Bindings (one descriptor table per slot, see GpuCrowdUpdater):
//   t0 : g_params  single CrowdParams element
//   u0 : g_agents  CrowdAgent array, updated in place
//

#define CROWD_MAX_CLIPS 8

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

struct CrowdParams
{
    float boundsMinX;
    float boundsMinZ;
    float boundsMaxX;
    float boundsMaxZ;
    float deltaTime;
    uint  agentCount;
    uint  clipCount;
    uint  threadsPerRow;   // threads along X of the dispatch (groupsX * 64)
    float clipDurations[CROWD_MAX_CLIPS];
};

StructuredBuffer<CrowdParams> g_params : register(t0);
RWStructuredBuffer<CrowdAgent> g_agents : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    CrowdParams p = g_params[0];
    uint index = id.x + id.y * p.threadsPerRow;
    if (index >= p.agentCount)
    {
        return;
    }

    CrowdAgent a = g_agents[index];
    float dt = p.deltaTime;

    precise float x = a.posX;
    precise float z = a.posZ;
    precise float vx = a.velX;
    precise float vz = a.velZ;

    precise float dx = vx * dt;
    x = x + dx;
    if (x < p.boundsMinX)
    {
        precise float overX = p.boundsMinX - x;
        x = p.boundsMinX + overX;
        vx = -vx;
    }
    else if (x > p.boundsMaxX)
    {
        precise float overX = x - p.boundsMaxX;
        x = p.boundsMaxX - overX;
        vx = -vx;
    }

    precise float dz = vz * dt;
    z = z + dz;
    if (z < p.boundsMinZ)
    {
        precise float overZ = p.boundsMinZ - z;
        z = p.boundsMinZ + overZ;
        vz = -vz;
    }
    else if (z > p.boundsMaxZ)
    {
        precise float overZ = z - p.boundsMaxZ;
        z = p.boundsMaxZ - overZ;
        vz = -vz;
    }

    uint clipCount = max(p.clipCount, 1u);
    uint clipIndex = min(a.clip, clipCount - 1u);
    float d = p.clipDurations[clipIndex];

    precise float t = a.animTime;
    precise float dtr = dt * a.animRate;
    t = t + dtr;
    if (t >= d)
    {
        t = t - d;
    }

    a.posX = x;
    a.posZ = z;
    a.velX = vx;
    a.velZ = vz;
    a.animTime = t;
    g_agents[index] = a;
}
