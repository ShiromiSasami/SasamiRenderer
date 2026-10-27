//
// SmokeVolume_CS.hlsl
// Eulerian smoke on a 3D grid using the Stable Fluids method
// (Stam, "Stable Fluids", SIGGRAPH 1999), following the GPU formulation in
// GPU Gems ch.38 (Harris, "Fast Fluid Dynamics Simulation on the GPU").
// One full step is four dispatches, driven from SmokeVolumeSim::Update():
//
//   1. AdvectMain    -- semi-Lagrangian advection of velocity and density,
//                       plus sources (emitter, buoyancy) and dissipation.
//   2. DivergenceMain -- divergence of the advected velocity field.
//   3. JacobiMain     -- one relaxation step of the pressure Poisson
//                       equation; dispatched N times, ping-ponging the
//                       pressure buffer (see SmokeVolumeSim::GetJacobiIterations).
//   4. ProjectMain    -- subtract the pressure gradient so the velocity
//                       field is (approximately) divergence-free.
//
// Storage mirrors FluidHeightfieldSim: plain structured buffers with
// ping-pong inline root descriptors instead of 3D textures, so the same
// root signature and binding model work for every stage. Cell index is
// x + y*countX + z*countX*countY throughout.
//
// Bindings (root descriptors):
//   b0 : SmokeUpdateCB   (root CBV)
//   t0 : g_stateIn       (current (velocity, density) state, root SRV)
//   t1 : g_pressureIn    (current (divergence, pressure) state, root SRV)
//   t2 : g_solidMask     (1 bit per cell, static obstacles, root SRV)
//   t3 : g_temperatureIn (current temperature, root SRV)
//   u0 : g_stateOut      (next state,    root UAV)
//   u1 : g_pressureOut   (next pressure, root UAV)
//   u2 : g_temperatureOut (next temperature, root UAV)
//
// Not every stage reads or writes all six buffers; all six are declared
// in every stage anyway so a single root signature serves the whole solver.
//

// Mirrors SmokeVolumeSim::SmokeUpdateCBData byte-for-byte.
cbuffer SmokeUpdateCB : register(b0)
{
    float    originX;
    float    originY;
    float    originZ;
    float    cellSize;

    uint     countX;
    uint     countY;
    uint     countZ;
    float    dt;

    float    emitterX;
    float    emitterY;
    float    emitterZ;
    float    emitterRadius;

    float    emitterDensity;
    float    emitterVelocity;
    float    buoyancy;
    float    dissipation;

    float    velocityDamping;
    float    vorticity;      // reserved; confinement is not implemented yet
    uint     emitterActive;
    uint     solidActive;    // nonzero = g_solidMask holds valid data and solid boundaries are applied

    float    windX;
    float    windY;
    float    windZ;
    float    windStrength;

    uint     buoyancyMode;          // 0 = density drives buoyancy (legacy), 1 = temperature (Fedkiw 2001 eq. 8)
    float    temperatureBuoyancy;   // beta
    float    densityWeight;         // alpha
    float    ambientTemperature;

    float    emitterTemperature;    // injected per second
    float    coolingRate;           // fraction of excess temperature lost per second
    float    reserved0;
    float    reserved1;
};

StructuredBuffer<float4>   g_stateIn     : register(t0); // xyz = velocity, w = density
StructuredBuffer<float2>   g_pressureIn  : register(t1); // x = divergence, y = pressure
StructuredBuffer<uint>     g_solidMask   : register(t2); // 1 bit per cell, always bound (>= 1 word)
RWStructuredBuffer<float4> g_stateOut    : register(u0);
RWStructuredBuffer<float2> g_pressureOut : register(u1);
StructuredBuffer<float>    g_temperatureIn  : register(t3); // advected scalar, read only by AdvectMain
RWStructuredBuffer<float>  g_temperatureOut : register(u2); // written by AdvectMain and ClearMain

uint CellIndex(uint3 c)
{
    return c.x + c.y * countX + c.z * countX * countY;
}

// Static obstacle test. The mask is built on the CPU from the scene's voxel occupancy
// (BuildSimGridSolidMask). Cells outside the grid are never solid: the domain edge keeps its own
// clamp and shell treatment, so the obstacle logic only has to describe interior geometry. With
// solidActive == 0 this is a single uniform branch returning false, so the stencils below take the
// exact same path as a build without obstacles.
bool IsSolid(int3 c)
{
    if (solidActive == 0)
    {
        return false;
    }
    if (any(c < int3(0, 0, 0)) || any(c >= int3((int)countX, (int)countY, (int)countZ)))
    {
        return false;
    }
    const uint i = CellIndex(uint3(c.x, c.y, c.z));
    return ((g_solidMask[i >> 5] >> (i & 31)) & 1u) != 0u;
}

// Clamps to the grid instead of wrapping or reading garbage outside the
// buffer -- the same Neumann-style (zero-gradient) edge treatment
// FluidHeightfield_CS uses for its border cells. Without this, both the
// back-trace in AdvectMain and the stencils in DivergenceMain/JacobiMain/
// ProjectMain would need branchy edge cases instead of a single clamp.
float4 LoadStateClamped(int3 c)
{
    int3 maxC = int3((int)countX - 1, (int)countY - 1, (int)countZ - 1);
    int3 cc = clamp(c, int3(0, 0, 0), maxC);
    return g_stateIn[CellIndex(uint3(cc.x, cc.y, cc.z))];
}

// Same Neumann-style clamp as LoadStateClamped, but over the pressure
// buffer: JacobiMain and ProjectMain both need a defined pressure just past
// the outer cells to build their stencils without special-casing the edges.
float LoadPressureClamped(int3 c)
{
    int3 maxC = int3((int)countX - 1, (int)countY - 1, (int)countZ - 1);
    int3 cc = clamp(c, int3(0, 0, 0), maxC);
    return g_pressureIn[CellIndex(uint3(cc.x, cc.y, cc.z))].y;
}

// Manual trilinear filtering over the 8 neighbouring cells. There is no
// hardware sampler here: the state lives in a StructuredBuffer rather than
// a Texture3D, which is the price of keeping the binding model identical to
// the rest of the renderer's root-descriptor-only compute shaders. Without
// this, semi-Lagrangian advection would snap to the nearest cell and the
// simulation would look blocky and diffuse the field far more than the
// scheme in Stam 1999 assumes.
float4 SampleStateTrilinear(float3 cellPos)
{
    float3 base = floor(cellPos);
    float3 frac = cellPos - base;
    int3 c0 = (int3)base;

    float4 c000 = LoadStateClamped(c0 + int3(0, 0, 0));
    float4 c100 = LoadStateClamped(c0 + int3(1, 0, 0));
    float4 c010 = LoadStateClamped(c0 + int3(0, 1, 0));
    float4 c110 = LoadStateClamped(c0 + int3(1, 1, 0));
    float4 c001 = LoadStateClamped(c0 + int3(0, 0, 1));
    float4 c101 = LoadStateClamped(c0 + int3(1, 0, 1));
    float4 c011 = LoadStateClamped(c0 + int3(0, 1, 1));
    float4 c111 = LoadStateClamped(c0 + int3(1, 1, 1));

    float4 x00 = lerp(c000, c100, frac.x);
    float4 x10 = lerp(c010, c110, frac.x);
    float4 x01 = lerp(c001, c101, frac.x);
    float4 x11 = lerp(c011, c111, frac.x);

    float4 y0 = lerp(x00, x10, frac.y);
    float4 y1 = lerp(x01, x11, frac.y);

    return lerp(y0, y1, frac.z);
}

// Same clamp as LoadStateClamped, over the temperature buffer.
float LoadTemperatureClamped(int3 c)
{
    int3 maxC = int3((int)countX - 1, (int)countY - 1, (int)countZ - 1);
    int3 cc = clamp(c, int3(0, 0, 0), maxC);
    return g_temperatureIn[CellIndex(uint3(cc.x, cc.y, cc.z))];
}

// Same 8-tap trilinear scheme as SampleStateTrilinear, over the temperature buffer.
float SampleTemperatureTrilinear(float3 cellPos)
{
    float3 base = floor(cellPos);
    float3 frac = cellPos - base;
    int3 c0 = (int3)base;

    float c000 = LoadTemperatureClamped(c0 + int3(0, 0, 0));
    float c100 = LoadTemperatureClamped(c0 + int3(1, 0, 0));
    float c010 = LoadTemperatureClamped(c0 + int3(0, 1, 0));
    float c110 = LoadTemperatureClamped(c0 + int3(1, 1, 0));
    float c001 = LoadTemperatureClamped(c0 + int3(0, 0, 1));
    float c101 = LoadTemperatureClamped(c0 + int3(1, 0, 1));
    float c011 = LoadTemperatureClamped(c0 + int3(0, 1, 1));
    float c111 = LoadTemperatureClamped(c0 + int3(1, 1, 1));

    float x00 = lerp(c000, c100, frac.x);
    float x10 = lerp(c010, c110, frac.x);
    float x01 = lerp(c001, c101, frac.x);
    float x11 = lerp(c011, c111, frac.x);

    float y0 = lerp(x00, x10, frac.y);
    float y1 = lerp(x01, x11, frac.y);

    return lerp(y0, y1, frac.z);
}

// Caps that keep the solve from running away. Both are empirical: the density cap is where smoke
// stops getting visibly denser, and kMaxCflCells is the Courant limit the explicit pressure solve
// tolerates -- one cell per step is the textbook bound, and staying under it leaves margin.
static const float kMaxDensity = 12.0f;
static const float kMaxTemperature = 12.0f;
static const float kMaxCflCells = 0.8f;

// Stage 1: semi-Lagrangian advection (Stam 1999, section 3.2) plus sources.
// Instead of stepping the field forward along characteristics (which is
// unconditionally unstable for the sizes of dt used in an interactive sim),
// each cell traces backwards along the current velocity to find where its
// content came from, and samples the old field there. This is unconditionally
// stable regardless of dt, at the cost of numerical dissipation -- which is
// why the CB also exposes an explicit dissipation/damping knob rather than
// relying solely on the scheme's own smoothing.
// Zeroes both buffers. The buffers are created with undefined content, and advection re-samples
// its own previous output every step -- so a single garbage float on frame one is not a transient
// artefact, it is a NaN that circulates forever and kills the whole field. Dispatched once after
// each (re)allocation rather than every frame.
[numthreads(8, 8, 8)]
void ClearMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= countX || id.y >= countY || id.z >= countZ) {
        return;
    }
    const uint i = CellIndex(id);
    g_stateOut[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_pressureOut[i] = float2(0.0f, 0.0f);
    g_temperatureOut[i] = ambientTemperature;
}

[numthreads(8, 8, 8)]
void AdvectMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= countX || id.y >= countY || id.z >= countZ)
    {
        return;
    }

    uint idx = CellIndex(id);

    // Obstacles are static (zero velocity) and hold no smoke. Writing zeros here also means the
    // trilinear back-trace of neighbouring cells reads zero from inside a solid, which is enough
    // to keep smoke from being advected out of the obstacle.
    if (IsSolid((int3)id))
    {
        g_temperatureOut[idx] = ambientTemperature;
        g_stateOut[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 self = g_stateIn[idx];
    float3 velocity = self.xyz;

    // Back-trace in cell space: dt and cellSize both have physical units,
    // so the velocity (world units / second) has to be converted to
    // cells / second before it can be subtracted from a cell-space position.
    float3 prevCellPos = (float3)id - velocity * dt / cellSize;
    float4 advected = SampleStateTrilinear(prevCellPos);

    float3 outVelocity = advected.xyz;
    float outDensity = advected.w;

    // Temperature rides the same back-trace as density and velocity. Its emitter injection and
    // cooling sit ahead of the buoyancy block so the force sees the current temperature; the
    // falloff is recomputed there (identical to the emitter block below) rather than moving the
    // buoyancy line, which would reorder the density arithmetic and change mode 0 output.
    float outTemperature = SampleTemperatureTrilinear(prevCellPos);
    if (emitterActive != 0)
    {
        float3 worldPos = float3(originX, originY, originZ) + (float3)id * cellSize;
        float dist = length(worldPos - float3(emitterX, emitterY, emitterZ));
        if (dist < emitterRadius)
        {
            float falloff = saturate(1.0f - dist / emitterRadius);
            falloff = falloff * falloff * (3.0f - 2.0f * falloff);
            outTemperature += emitterTemperature * dt * falloff;
        }
    }
    // Fedkiw, Stam & Jensen 2001: exponential relaxation towards the ambient temperature.
    outTemperature = ambientTemperature + (outTemperature - ambientTemperature) * exp(-coolingRate * dt);

    // Buoyancy: hot smoke rises. Mode 0 keeps the legacy single-field simplification where density
    // doubles as the buoyancy driver (denser smoke is treated as hotter/lighter). Any other mode
    // uses the separate temperature field (Boussinesq approximation).
    if (buoyancyMode == 0u)
    {
        outVelocity += float3(0.0f, buoyancy * outDensity * dt, 0.0f);
    }
    else
    {
        // Fedkiw, Stam & Jensen 2001, "Visual Simulation of Smoke", eq. 8:
        // f_buoy = -alpha * rho * z + beta * (T - T_amb) * z
        const float force = -densityWeight * outDensity + temperatureBuoyancy * (outTemperature - ambientTemperature);
        outVelocity += float3(0.0f, force * dt, 0.0f);
    }

    if (emitterActive != 0)
    {
        float3 worldPos = float3(originX, originY, originZ) + (float3)id * cellSize;
        float dist = length(worldPos - float3(emitterX, emitterY, emitterZ));
        if (dist < emitterRadius)
        {
            // Smooth falloff instead of a hard cutoff: a binary radius test
            // would inject density/velocity discontinuously as cells cross
            // the boundary, visible as a popping sphere each frame.
            float falloff = saturate(1.0f - dist / emitterRadius);
            falloff = falloff * falloff * (3.0f - 2.0f * falloff); // smoothstep-style ease
            outDensity += emitterDensity * dt * falloff;
            outVelocity += float3(0.0f, emitterVelocity * dt * falloff, 0.0f);
        }
    }

    // Explicit dissipation/damping (GPU Gems 38): the semi-Lagrangian step
    // already loses some energy to interpolation, but that implicit loss is
    // resolution-dependent, so an explicit, tunable decay is layered on top
    // to get a controllable falloff.
    outDensity *= saturate(1.0f - dissipation * dt);
    outVelocity *= saturate(1.0f - velocityDamping * dt);

    // Ambient wind, as a force toward the target velocity rather than a replacement for it: a
    // replacement would erase the plume's own motion and the smoke would simply translate. The
    // force is scaled by dt so the result does not depend on frame rate.
    if (windStrength > 0.0f) {
        const float3 windVelocity = float3(windX, windY, windZ) * windStrength;
        outVelocity += (windVelocity - outVelocity) * saturate(dt * 1.5f);
    }

    // --- Divergence guards. Observed on 2026-09-15: the plume spread far too fast, then the whole
    // --- field froze and the smoke silently vanished. Frozen-and-invisible is the signature of NaN
    // --- here, because `density > threshold` is false for NaN, so the raymarch draws nothing and
    // --- the picture stops changing. Three separate causes, all of them unbounded growth:

    // 1. The emitter adds density every frame with nothing to stop it. Density feeds buoyancy,
    //    buoyancy feeds velocity, and the back-trace then samples further than the grid is wide.
    //    Real smoke saturates; cap it.
    outDensity = clamp(outDensity, 0.0f, kMaxDensity);
    outTemperature = clamp(outTemperature, ambientTemperature - kMaxTemperature, ambientTemperature + kMaxTemperature);

    // 2. Semi-Lagrangian advection is unconditionally stable, but the explicit Jacobi pressure
    //    solve is not, and a velocity that moves more than a few cells per step leaves it with a
    //    divergence it cannot remove. Cap the speed rather than the timestep, which would make the
    //    simulation slow down instead of stabilising.
    const float speed = length(outVelocity);
    const float maxSpeed = kMaxCflCells * cellSize / max(dt, 1e-5f);
    if (speed > maxSpeed) {
        outVelocity *= maxSpeed / speed;
    }

    // 3. Self-healing. Advection re-samples its own output, so one non-finite cell spreads and
    //    never leaves -- restoring the parameters that produced it does not help, which is exactly
    //    what was seen. Resetting the cell costs one comparison and turns a permanent kill into a
    //    local, transient hole. isfinite() rather than isnan() so infinities are caught too.
    if (!all(isfinite(outVelocity)) || !isfinite(outDensity)) {
        outVelocity = float3(0.0f, 0.0f, 0.0f);
        outDensity = 0.0f;
    }
    if (!isfinite(outTemperature)) {
        outTemperature = ambientTemperature;
    }

    g_stateOut[idx] = float4(outVelocity, outDensity);
    g_temperatureOut[idx] = outTemperature;
}

// Stage 2: divergence of the advected velocity field, the right-hand side
// of the pressure Poisson equation (Stam 1999, section 3.4 / GPU Gems 38).
[numthreads(8, 8, 8)]
void DivergenceMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= countX || id.y >= countY || id.z >= countZ)
    {
        return;
    }

    uint idx = CellIndex(id);

    if (IsSolid((int3)id))
    {
        g_pressureOut[idx] = float2(0.0f, 0.0f);
        return;
    }

    float4 left  = LoadStateClamped((int3)id + int3(-1, 0, 0));
    float4 right = LoadStateClamped((int3)id + int3(1, 0, 0));
    float4 down  = LoadStateClamped((int3)id + int3(0, -1, 0));
    float4 up    = LoadStateClamped((int3)id + int3(0, 1, 0));
    float4 back  = LoadStateClamped((int3)id + int3(0, 0, -1));
    float4 front = LoadStateClamped((int3)id + int3(0, 0, 1));

    // A solid neighbour contributes the obstacle's velocity, which is zero for a static obstacle
    // (GPU Gems 3 ch.30, section 30.2; Crane, Llamas & Tariq 2007).
    const float4 zeroState = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (IsSolid((int3)id + int3(-1, 0, 0))) { left  = zeroState; }
    if (IsSolid((int3)id + int3(1, 0, 0)))  { right = zeroState; }
    if (IsSolid((int3)id + int3(0, -1, 0))) { down  = zeroState; }
    if (IsSolid((int3)id + int3(0, 1, 0)))  { up    = zeroState; }
    if (IsSolid((int3)id + int3(0, 0, -1))) { back  = zeroState; }
    if (IsSolid((int3)id + int3(0, 0, 1)))  { front = zeroState; }

    float divergence = 0.5f * ((right.x - left.x) + (up.y - down.y) + (front.z - back.z)) / cellSize;

    g_pressureOut[idx] = float2(divergence, 0.0f); // pressure starts at zero for the first Jacobi pass
}

// Stage 3: one Jacobi relaxation step for the pressure Poisson equation
// laplacian(p) = divergence (Stam 1999, section 3.4). Dispatched repeatedly
// by the CPU side (SmokeVolumeSim::GetJacobiIterations), ping-ponging
// g_pressureIn/g_pressureOut each time; more iterations converge closer to
// a truly divergence-free field, at linearly increasing cost -- GPU Gems 38
// notes 40-80 iterations for high quality, and that ~20 already reads
// convincingly as smoke.
[numthreads(8, 8, 8)]
void JacobiMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= countX || id.y >= countY || id.z >= countZ)
    {
        return;
    }

    uint idx = CellIndex(id);
    int3 c = (int3)id;

    if (IsSolid(c))
    {
        g_pressureOut[idx] = float2(0.0f, 0.0f);
        return;
    }

    float2 self = g_pressureIn[idx];
    float divergence = self.x;

    float pLeft  = LoadPressureClamped(c + int3(-1, 0, 0));
    float pRight = LoadPressureClamped(c + int3(1, 0, 0));
    float pDown  = LoadPressureClamped(c + int3(0, -1, 0));
    float pUp    = LoadPressureClamped(c + int3(0, 1, 0));
    float pBack  = LoadPressureClamped(c + int3(0, 0, -1));
    float pFront = LoadPressureClamped(c + int3(0, 0, 1));

    // Pure Neumann condition at an obstacle face, dp/dn = 0 (GPU Gems 3 ch.30, section 30.2): the
    // solid neighbour's pressure is replaced by the centre cell's own, so the face contributes no
    // gradient and the Poisson solve sees a wall.
    if (IsSolid(c + int3(-1, 0, 0))) { pLeft  = self.y; }
    if (IsSolid(c + int3(1, 0, 0)))  { pRight = self.y; }
    if (IsSolid(c + int3(0, -1, 0))) { pDown  = self.y; }
    if (IsSolid(c + int3(0, 1, 0)))  { pUp    = self.y; }
    if (IsSolid(c + int3(0, 0, -1))) { pBack  = self.y; }
    if (IsSolid(c + int3(0, 0, 1)))  { pFront = self.y; }

    float pressure = (pLeft + pRight + pDown + pUp + pBack + pFront - divergence * cellSize * cellSize) / 6.0f;

    // Divergence is carried through unchanged: it is the fixed right-hand
    // side of the Poisson equation, and every subsequent Jacobi pass still
    // needs it even though this pass only updates pressure.
    g_pressureOut[idx] = float2(divergence, pressure);
}

// Curl of the velocity field at a cell, by central differences. Vorticity is what a fluid's
// small-scale swirl *is*, and semi-Lagrangian advection is famous for destroying it: every step
// interpolates, interpolation smooths, and the eddies that make smoke look like smoke average away
// within a few frames. That is the numerical dissipation Fedkiw, Stam and Jensen set out to undo in
// "Visual Simulation of Smoke" (SIGGRAPH 2001).
float3 CurlAt(int3 c)
{
    const float scale = 0.5f / cellSize;
    const float3 right = LoadStateClamped(c + int3(1, 0, 0)).xyz;
    const float3 left  = LoadStateClamped(c + int3(-1, 0, 0)).xyz;
    const float3 up    = LoadStateClamped(c + int3(0, 1, 0)).xyz;
    const float3 down  = LoadStateClamped(c + int3(0, -1, 0)).xyz;
    const float3 front = LoadStateClamped(c + int3(0, 0, 1)).xyz;
    const float3 back  = LoadStateClamped(c + int3(0, 0, -1)).xyz;

    return float3(
        (up.z - down.z) - (front.y - back.y),
        (front.x - back.x) - (right.z - left.z),
        (right.y - left.y) - (up.x - down.x)) * scale;
}

// Vorticity confinement force, Fedkiw et al. 2001 eq. (11)-(12):
//
//   N = grad|omega| / |grad|omega||      f = epsilon * h * (N x omega)
//
// N points from low to high vorticity, so the cross product pushes velocity around the centre of
// each eddy -- feeding energy back into exactly the rotation the advection step drained, and only
// there. It is not physical: epsilon is a dial for how much of the lost detail to put back, and the
// h factor makes the added energy vanish as the grid is refined, so the result converges to the
// true solution rather than to a permanently over-swirled one.
float3 VorticityConfinement(int3 c, float3 omega)
{
    const float omegaLength = length(omega);
    if (omegaLength < 1e-5f) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // grad|omega| needs |omega| at the neighbours, and each of those needs its own neighbours --
    // a two-ring stencil. Done inline rather than through a scratch buffer because the pressure
    // pair is fully occupied and adding a third buffer for one intermediate is not worth it.
    const float scale = 0.5f / cellSize;
    float3 eta;
    eta.x = (length(CurlAt(c + int3(1, 0, 0))) - length(CurlAt(c + int3(-1, 0, 0)))) * scale;
    eta.y = (length(CurlAt(c + int3(0, 1, 0))) - length(CurlAt(c + int3(0, -1, 0)))) * scale;
    eta.z = (length(CurlAt(c + int3(0, 0, 1))) - length(CurlAt(c + int3(0, 0, -1)))) * scale;

    const float etaLength = length(eta);
    if (etaLength < 1e-5f) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 N = eta / etaLength;
    return vorticity * cellSize * cross(N, omega);
}

// Stage 4: subtract the pressure gradient from the advected velocity so the
// result is (approximately) divergence-free (Stam 1999, section 3.5 / GPU
// Gems 38's projection step). Density passes through untouched -- pressure
// only acts on velocity.
[numthreads(8, 8, 8)]
void ProjectMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= countX || id.y >= countY || id.z >= countZ)
    {
        return;
    }

    uint idx = CellIndex(id);
    int3 c = (int3)id;

    if (IsSolid(c))
    {
        g_stateOut[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 self = g_stateIn[idx];

    float pCenter = g_pressureIn[idx].y;
    float pLeft  = LoadPressureClamped(c + int3(-1, 0, 0));
    float pRight = LoadPressureClamped(c + int3(1, 0, 0));
    float pDown  = LoadPressureClamped(c + int3(0, -1, 0));
    float pUp    = LoadPressureClamped(c + int3(0, 1, 0));
    float pBack  = LoadPressureClamped(c + int3(0, 0, -1));
    float pFront = LoadPressureClamped(c + int3(0, 0, 1));

    // Same Neumann substitution as JacobiMain, so the gradient subtracted here matches the
    // pressure field the solver actually converged to.
    const bool solidLeft  = IsSolid(c + int3(-1, 0, 0));
    const bool solidRight = IsSolid(c + int3(1, 0, 0));
    const bool solidDown  = IsSolid(c + int3(0, -1, 0));
    const bool solidUp    = IsSolid(c + int3(0, 1, 0));
    const bool solidBack  = IsSolid(c + int3(0, 0, -1));
    const bool solidFront = IsSolid(c + int3(0, 0, 1));
    if (solidLeft)  { pLeft  = pCenter; }
    if (solidRight) { pRight = pCenter; }
    if (solidDown)  { pDown  = pCenter; }
    if (solidUp)    { pUp    = pCenter; }
    if (solidBack)  { pBack  = pCenter; }
    if (solidFront) { pFront = pCenter; }

    float3 gradient;
    gradient.x = (pRight - pLeft) * (0.5f / cellSize);
    gradient.y = (pUp - pDown) * (0.5f / cellSize);
    gradient.z = (pFront - pBack) * (0.5f / cellSize);

    float3 velocity = self.xyz - gradient;

    // Confinement is applied here, after the projection, using the pre-projection field this stage
    // already has bound -- which saves a whole extra ping-pong stage for one force. The cost is
    // that the result carries a little divergence into the next frame, where the next projection
    // removes it; the force is small enough that the error never accumulates.
    if (vorticity > 0.0f) {
        velocity += VorticityConfinement(c, CurlAt(c)) * dt;
    }

    // Obstacle boundary: no through-flow into a solid neighbour, while flow along or away from it
    // is kept (free-slip) -- the same rule the domain-shell code below applies at the grid edge.
    // GPU Gems 3 ch.30 instead forces the whole component to the obstacle velocity; zeroing only
    // the inward part keeps smoke from sticking to walls.
    if ((solidLeft && velocity.x < 0.0f) || (solidRight && velocity.x > 0.0f))
    {
        velocity.x = 0.0f;
    }
    if ((solidDown && velocity.y < 0.0f) || (solidUp && velocity.y > 0.0f))
    {
        velocity.y = 0.0f;
    }
    if ((solidBack && velocity.z < 0.0f) || (solidFront && velocity.z > 0.0f))
    {
        velocity.z = 0.0f;
    }

    // Solid boundary: zero the velocity component that points out of the
    // grid at the outer shell of cells. Without this the projection has
    // nothing to push against at the domain edge, so smoke (and mass with
    // it) simply leaks out of the volume instead of being contained by it.
    if ((id.x == 0 && velocity.x < 0.0f) || (id.x == countX - 1u && velocity.x > 0.0f))
    {
        velocity.x = 0.0f;
    }
    if ((id.y == 0 && velocity.y < 0.0f) || (id.y == countY - 1u && velocity.y > 0.0f))
    {
        velocity.y = 0.0f;
    }
    if ((id.z == 0 && velocity.z < 0.0f) || (id.z == countZ - 1u && velocity.z > 0.0f))
    {
        velocity.z = 0.0f;
    }

    g_stateOut[idx] = float4(velocity, self.w);
}
