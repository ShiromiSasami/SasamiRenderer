#ifndef SASAMI_RAYMARCH_WATER_SCENE_HLSLI
#define SASAMI_RAYMARCH_WATER_SCENE_HLSLI

// Water surface, scene SDF, normals, shadows, and AO helpers.

float2 CamOrigin()
{
    return floor(u_camPos.xz / kFloatOriginCell) * kFloatOriginCell;
}

// World XZ 竊・stable local XZ ([0, 1024) when camera is inside the cell).
float2 MakeStableXZ(float2 pWS_xz)
{
    return pWS_xz - CamOrigin();
}

// Camera in stable local coordinates (always small).
float2 CamLocalXZ()
{
    return u_camPos.xz - CamOrigin();
}

// ---------------------------------------------------------------------------
// Wave / fluid helpers
// ---------------------------------------------------------------------------

// Distance-from-CAMERA LOD weights for mid and high frequency wave octaves.
// p_stable = MakeStableXZ(worldXZ); both p_stable and CamLocalXZ() are small,
// so the subtraction is exact (no catastrophic cancellation).
void waveLod(float2 p_stable, out float midW, out float hiW)
{
    float dist = length(p_stable - CamLocalXZ());
    // Mid-freq octave (2.80, 2.10 cyc/u): fade 160-320 u from camera
    midW = saturate(1.0f - (dist - 160.0f) / 160.0f);
    // High-freq octaves (4.10, 6.50 cyc/u): fade 80-160 u from camera
    hiW  = saturate(1.0f - (dist -  80.0f) /  80.0f);
}

// Multi-octave wave height.
// p = MakeStableXZ(worldXZ) 窶・inputs are always < 1024 u 竊・precise sin arguments.
float waveHeight(float2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);

    float h = 0.0f;
    h += sin(p.x * 1.40f                    + t * 1.80f) * 0.180f;
    h += sin(p.y * 1.90f                    + t * 1.40f) * 0.140f;
    h += sin(p.x * 0.70f - p.y * 1.10f     + t * 0.90f) * 0.090f;
    h += sin(p.x * 2.80f + p.y * 2.10f     - t * 2.30f) * 0.048f * midW;
    h += sin(p.x * 4.10f - p.y * 3.30f     + t * 3.10f) * 0.024f * hiW;
    h += sin(p.x * 6.50f + p.y * 5.20f     - t * 4.20f) * 0.012f * hiW;
    return h;
}

// Water surface SDF 窶・perturbed horizontal plane at y = 0.
// Multiply by 0.75 to keep it a conservative underestimate when wave gradient > 0.
float sdWater(float3 p)
{
    return (p.y - waveHeight(MakeStableXZ(p.xz), u_time)) * 0.75f;
}

// Analytic wave surface normal (gradient of waveHeight).
// p = MakeStableXZ(worldXZ) 窶・same stable coordinates as waveHeight.
float3 waveNormal(float2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);

    float dhdx = 0.0f;
    float dhdz = 0.0f;
    dhdx += cos(p.x * 1.40f                    + t * 1.80f) * 1.40f * 0.180f;
    dhdz += cos(p.y * 1.90f                    + t * 1.40f) * 1.90f * 0.140f;
    dhdx += cos(p.x * 0.70f - p.y * 1.10f     + t * 0.90f) *  0.70f * 0.090f;
    dhdz += cos(p.x * 0.70f - p.y * 1.10f     + t * 0.90f) * -1.10f * 0.090f;
    dhdx += cos(p.x * 2.80f + p.y * 2.10f     - t * 2.30f) *  2.80f * 0.048f * midW;
    dhdz += cos(p.x * 2.80f + p.y * 2.10f     - t * 2.30f) *  2.10f * 0.048f * midW;
    dhdx += cos(p.x * 4.10f - p.y * 3.30f     + t * 3.10f) *  4.10f * 0.024f * hiW;
    dhdz += cos(p.x * 4.10f - p.y * 3.30f     + t * 3.10f) * -3.30f * 0.024f * hiW;
    dhdx += cos(p.x * 6.50f + p.y * 5.20f     - t * 4.20f) *  6.50f * 0.012f * hiW;
    dhdz += cos(p.x * 6.50f + p.y * 5.20f     - t * 4.20f) *  5.20f * 0.012f * hiW;
    return normalize(float3(-dhdx, 1.0f, -dhdz));
}

// ---------------------------------------------------------------------------
// SDF primitives
// ---------------------------------------------------------------------------
float sdSphere(float3 p, float r)
{
    return length(p) - r;
}

float sdRoundBox(float3 p, float3 b, float r)
{
    float3 q = abs(p) - b;
    return length(max(q, 0.0f)) + min(max(q.x, max(q.y, q.z)), 0.0f) - r;
}

float sdCapsule(float3 p, float3 a, float3 b, float r)
{
    float3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0f, 1.0f);
    return length(pa - ba * h) - r;
}

// ---------------------------------------------------------------------------
// Scene map: returns float2(dist, materialId)
//   matId 0 = water surface (animated waves)
//   matId 1 = sphere (orange-red)
//   matId 2 = box    (blue-grey)
//   matId 3 = capsule (green, partial above water)
// ---------------------------------------------------------------------------
float2 map(float3 p)
{
    float bob = sin(u_time * 0.6f) * 0.18f;

    // Water surface
    float dWater = sdWater(p);

    // Orange sphere, bobbing above water
    float3 pSphere = p - float3(-1.8f, 2.0f + bob, 0.5f);
    float  dSphere = sdSphere(pSphere, 1.0f);

    // Blue rounded box, slightly rotating around its axis (fake with a wobble)
    float  angle   = u_time * 0.3f;
    float3 pBox    = p - float3(2.5f, 1.2f, -0.3f);
    // Approximate rotation by shearing 窶・keeps SDF mostly valid
    pBox.xz = float2(pBox.x * cos(angle) + pBox.z * sin(angle),
                    -pBox.x * sin(angle) + pBox.z * cos(angle));
    float dBox = sdRoundBox(pBox, float3(0.75f, 0.65f, 0.75f), 0.12f);

    // Green capsule emerging from water
    float3 capA = float3(-4.2f, -0.3f,  0.8f);
    float3 capB = float3(-4.2f,  2.8f,  0.8f);
    float  dCap = sdCapsule(p, capA, capB, 0.38f);

    // Union (start with water)
    float2 res = float2(dWater, 0.0f);
    if (dSphere < res.x) res = float2(dSphere, 1.0f);
    if (dBox    < res.x) res = float2(dBox,    2.0f);
    if (dCap    < res.x) res = float2(dCap,    3.0f);
    return res;
}

// ---------------------------------------------------------------------------
// Normal (gradient via finite differences 窶・used for solid objects)
// ---------------------------------------------------------------------------
float3 calcNormal(float3 p)
{
    float2 e = float2(kEps, 0.0f);
    return normalize(float3(
        map(p + e.xyy).x - map(p - e.xyy).x,
        map(p + e.yxy).x - map(p - e.yxy).x,
        map(p + e.yyx).x - map(p - e.yyx).x));
}

// ---------------------------------------------------------------------------
// Soft shadow
// ---------------------------------------------------------------------------
float softShadow(float3 ro, float3 rd, float mint, float maxt, float k)
{
    float res = 1.0f;
    float t   = mint;
    for (int i = 0; i < 32; ++i) {
        float h = map(ro + rd * t).x;
        if (h < 0.001f) return 0.0f;
        res = min(res, k * h / t);
        t  += clamp(h, 0.01f, 0.4f);
        if (t > maxt) break;
    }
    return clamp(res, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Ambient occlusion
// ---------------------------------------------------------------------------
float calcAO(float3 pos, float3 nor)
{
    float occ = 0.0f, sca = 1.0f;
    for (int i = 0; i < 5; i++) {
        float h = 0.01f + 0.12f * float(i) / 4.0f;
        float d = map(pos + h * nor).x;
        occ    += (h - d) * sca;
        sca    *= 0.95f;
    }
    return clamp(1.0f - 3.0f * occ, 0.0f, 1.0f);
}

#endif // SASAMI_RAYMARCH_WATER_SCENE_HLSLI
