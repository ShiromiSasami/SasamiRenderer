// RayMarch_PS.hlsl
// SDF raymarching demo: sphere, box, capsule, animated water surface + procedural sky.
//
// CB layout matches PushCameraCB (5-arg form):
//   u_invVP           -> camera inverse view-projection (row-major, compatibility only)
//   world[ 0.. 3]     -> camPos.xyz, time
//   world[ 4.. 7]     -> sunDir.xyz, sunIntensity
//   world[ 8..11]     -> sunColor.rgb, cloudCover
//   world[12..15]     -> renderW, renderH, reserved, cloudDensity
//   extra0[4]         -> debugMode, tanHalfFovX, tanHalfFovY, rayMarchCameraEnabled
//   extra1[4]         -> cameraRight.xyz, reserved
//   extra2[4]         -> cameraUp.xyz, reserved

cbuffer RayMarchCB : register(b0)
{
    row_major float4x4 u_invVP;

    float3 u_camPos;      float u_time;
    float3 u_sunDir;      float u_sunI;
    float3 u_sunColor;    float u_cloudCover;
    float  u_renderW;     float u_renderH; float u_fluidMode; float u_cloudDensity;

    float4 u_extra0;
    float4 u_extra1;
    float4 u_extra2;
}

static const float kEps      = 0.0005f;
static const float kMaxDist  = 300.0f;   // increased for better coverage when camera is elevated
static const int   kMaxSteps = 512;      // increased to prevent step exhaustion at large distances
static const float PI = 3.14159265f;

// ---------------------------------------------------------------------------
// Floating Origin helpers
// ---------------------------------------------------------------------------
// Snap to a 1024 m grid so that wave/noise inputs are always small (< 1024 u).
// This prevents float32 catastrophic cancellation at large world coordinates
// (cam.x ~ 10^6 → ULP ≈ 0.125 u → visible wave quantisation).
//
// All wave / noise functions now take "stable XZ" = MakeStableXZ(worldXZ).
// sdWater and shadeWater call MakeStableXZ once and pass the result down,
// keeping the SDF comparison in world Y (unchanged).
static const float kFloatOriginCell = 1024.0f;

float2 CamOrigin()
{
    return floor(u_camPos.xz / kFloatOriginCell) * kFloatOriginCell;
}

// World XZ → stable local XZ ([0, 1024) when camera is inside the cell).
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
// p = MakeStableXZ(worldXZ) — inputs are always < 1024 u → precise sin arguments.
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

// Water surface SDF — perturbed horizontal plane at y = 0.
// Multiply by 0.75 to keep it a conservative underestimate when wave gradient > 0.
float sdWater(float3 p)
{
    return (p.y - waveHeight(MakeStableXZ(p.xz), u_time)) * 0.75f;
}

// Analytic wave surface normal (gradient of waveHeight).
// p = MakeStableXZ(worldXZ) — same stable coordinates as waveHeight.
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
    // Approximate rotation by shearing — keeps SDF mostly valid
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
// Normal (gradient via finite differences — used for solid objects)
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

// ---------------------------------------------------------------------------
// Procedural sky: analytic atmosphere + FBM volumetric clouds
// (matches ProceduralSky_PS.hlsl implementation)
// ---------------------------------------------------------------------------
// Gradient noise (Perlin-style) — replaces value noise to eliminate cell-boundary seams.
// Value noise interpolates random *values* at grid corners; gradient noise interpolates
// random *dot products* (gradient · offset), which is C1 at cell boundaries by construction.
// Combined with quintic (C2) interpolation, seams between cells become invisible.

float3 GradHash3(float3 p)
{
    // Map each integer cell to a pseudo-random unit gradient vector.
    p = float3(dot(p, float3(127.1f, 311.7f,  74.7f)),
               dot(p, float3(269.5f, 183.3f, 246.1f)),
               dot(p, float3(113.5f, 271.9f, 124.6f)));
    float3 g = frac(sin(p) * 43758.5453f) * 2.0f - 1.0f;
    return g / (length(g) + 1e-5f); // safe normalize
}

// 3-D gradient noise in [-1, 1].
// Uses quintic smoothstep (6t^5-15t^4+10t^3) which is C2 continuous,
// eliminating the second-derivative discontinuity that makes Hermite (3t^2-2t^3)
// show subtle grid lines even in value noise.
float GradNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // quintic

    return lerp(
        lerp(lerp(dot(GradHash3(i),                 f),
                  dot(GradHash3(i + float3(1,0,0)), f - float3(1,0,0)), u.x),
             lerp(dot(GradHash3(i + float3(0,1,0)), f - float3(0,1,0)),
                  dot(GradHash3(i + float3(1,1,0)), f - float3(1,1,0)), u.x), u.y),
        lerp(lerp(dot(GradHash3(i + float3(0,0,1)), f - float3(0,0,1)),
                  dot(GradHash3(i + float3(1,0,1)), f - float3(1,0,1)), u.x),
             lerp(dot(GradHash3(i + float3(0,1,1)), f - float3(0,1,1)),
                  dot(GradHash3(i + float3(1,1,1)), f - float3(1,1,1)), u.x), u.y), u.z);
}

float FBM5(float3 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll]
    for (int i = 0; i < 5; i++) {
        v += a * (GradNoise(p) * 0.5f + 0.5f); // remap [-1,1] -> [0,1]
        p = p * 2.02f + float3(73.1f, 61.4f, 53.7f);
        a *= 0.5f;
    }
    return v;
}

float SampleCloudDensity(float3 worldPos, float time, float cloudCover, float cloudDensity)
{
    // Scale worldPos so XYZ variation is comparable in noise space.
    // Y (altitude 1500-5000m) previously only mapped to 0.15-0.5 (range 0.35),
    // while XZ varied ~4x more depending on view angle → horizontal sheets / banding.
    // heightScale=4 expands Y to 0.6-2.0 (range 1.4) — proper 3D volumetric variation.
    const float heightScale = 4.0f;
    float3 p = float3(worldPos.x, worldPos.y * heightScale, worldPos.z) * 0.0001f;
    p.x += time * 0.008f;
    p.z += time * 0.003f;
    float base   = FBM5(p * 0.8f);
    float detail = FBM5(p * 2.5f + float3(1.4f, 2.1f, 0.9f) + time * 0.002f) * 0.35f;
    float raw = base + detail - (1.0f - cloudCover);
    return max(raw * cloudDensity, 0.0f);
}

float4 MarchClouds(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity,
                   float time, float cloudCover, float cloudDensity, float jitter)
{
    const float kCloudBaseY = 1500.0f;
    const float kCloudTopY  = 5000.0f;
    if (rd.y < 0.005f) return float4(0, 0, 0, 0);
    float tBase = (kCloudBaseY - u_camPos.y) / rd.y;
    float tTop  = (kCloudTopY  - u_camPos.y) / rd.y;
    if (tTop <= 0.0f) return float4(0, 0, 0, 0);
    tBase = max(tBase, 0.0f);
    if (tBase > 300000.0f) return float4(0, 0, 0, 0);

    const int kSteps = 48;
    float dt = (tTop - tBase) / float(kSteps);

    float g = 0.6f, g2 = g * g;
    float cosSun = dot(rd, sunDir);
    float denom  = max(1.0f + g2 - 2.0f * g * cosSun, 1e-6f);
    float phaseHG = (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));

    float transmittance = 1.0f;
    float3 scattered = 0.0f;

    for (int i = 0; i < kSteps; i++) {
        if (transmittance < 0.02f) break;
        // Per-pixel jitter offsets each ray's start position within the first step,
        // breaking the fixed-step "shell" pattern that causes visible horizontal bands.
        float  t   = tBase + (float(i) + jitter) * dt;
        // Use true world-space positions for the cloud slab. The cloud noise is
        // evaluated at a very low frequency (1e-4 scale), so full world coords
        // remain stable while avoiding the 1024m floating-origin wrap used by water.
        float3 pos = u_camPos + rd * t;
        float density = SampleCloudDensity(pos, time, cloudCover, cloudDensity);
        if (density < 1e-4f) continue;
        float sigmaE = density * 0.0008f;
        float sampleT = exp(-sigmaE * dt);
        float sunAtten = exp(-density * 1.2f);
        float3 sunLit  = sunColor * sunIntensity * sunAtten * phaseHG;
        float3 ambient = float3(0.35f, 0.50f, 0.72f) * 0.28f;
        float3 inScatter = (sunLit + ambient) * density;
        scattered += inScatter * transmittance * (1.0f - sampleT) / max(sigmaE, 1e-6f);
        transmittance *= sampleT;
    }
    return float4(scattered, 1.0f - transmittance);
}

float3 ComputeSkyColor(float3 rd, float3 sunDir, float3 sunColor, float sunIntensity)
{
    float cosTheta  = dot(rd, sunDir);
    float elevation = rd.y;

    float3 zenithColor  = float3(0.05f, 0.15f, 0.65f) * 2.0f;
    float3 horizonColor = float3(0.50f, 0.68f, 0.88f) * 1.5f;
    float3 groundColor  = float3(0.08f, 0.06f, 0.04f);

    float3 skyBase;
    if (elevation >= 0.0f)
        skyBase = lerp(horizonColor, zenithColor, pow(saturate(elevation), 0.4f));
    else
        skyBase = lerp(horizonColor, groundColor, saturate(-elevation * 5.0f));

    float sunInfluence = saturate(cosTheta * 0.5f + 0.5f);
    float3 warmTint = lerp(float3(1,1,1), sunColor * float3(1.1f, 0.9f, 0.7f), sunInfluence * 0.4f);
    skyBase *= warmTint;

    float mie = pow(max(cosTheta, 0.0f), 6.0f) * 0.3f;
    float3 mieColor = sunColor * mie * sunIntensity;

    float horizonFactor = pow(saturate(1.0f - abs(elevation)), 3.0f);
    float3 horizGlow = sunColor
        * lerp(float3(1.0f, 0.7f, 0.4f), float3(1.0f, 0.9f, 0.7f), sunIntensity)
        * horizonFactor * max(cosTheta, 0.0f) * 0.4f * sunIntensity;

    float sunDisc = smoothstep(0.9996f, 1.0f, cosTheta);
    float3 disc = sunColor * sunDisc * 12.0f * sunIntensity;

    return max(skyBase + mieColor + horizGlow + disc, 0.0f);
}

// Lightweight sky for water reflections (no cloud marching)
float3 skyColor(float3 rd)
{
    return ComputeSkyColor(rd, normalize(u_sunDir), u_sunColor, u_sunI);
}

// ---------------------------------------------------------------------------
// Water shading
// ---------------------------------------------------------------------------
float3 shadeWater(float3 pos, float3 rd, float tHit)
{
    // Precision-safe stable XZ: CamLocalXZ() + rd.xz*tHit keeps both operands
    // small.  MakeStableXZ(pos.xz) would cancel (pos.xz = large + large).
    float2 stableXZ = CamLocalXZ() + rd.xz * tHit;
    float3 N   = waveNormal(stableXZ, u_time);
    float3 V   = -rd;
    float3 sunDir = normalize(u_sunDir);

    float NdotV = saturate(dot(N, V));

    // Schlick Fresnel (F0=0.02 for water at normal incidence)
    float fresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);

    // Reflected sky
    float3 reflDir  = reflect(rd, N);
    float3 reflColor = skyColor(reflDir);

    // Water depth absorption color
    float3 deepColor    = float3(0.00f, 0.08f, 0.18f);
    float3 shallowColor = float3(0.04f, 0.28f, 0.42f);
    float3 waterColor   = lerp(deepColor, shallowColor, pow(NdotV, 1.5f));

    // Whitecap foam at wave crests
    float wh   = waveHeight(stableXZ, u_time);
    float foam = smoothstep(0.22f, 0.40f, wh);

    // Subsurface ambient (tint from sky overhead)
    float3 skyAmb = skyColor(float3(0.0f, 1.0f, 0.0f)) * 0.20f;
    waterColor   += skyAmb * (1.0f - fresnel);

    // Caustic shimmer (cheap: use wave height + view angle)
    float caustic = pow(saturate(dot(N, float3(0.0f, 1.0f, 0.0f))), 32.0f)
                  * smoothstep(0.0f, 0.06f, wh + 0.06f) * 0.4f;
    waterColor   += float3(0.3f, 0.6f, 0.8f) * caustic;

    // Sun specular highlight
    float3 H    = normalize(sunDir + V);
    float  spec = pow(max(0.0f, dot(N, H)), 512.0f) * u_sunI;

    // Soft shadow from objects above water
    float shadow = softShadow(pos + float3(0.0f, 0.01f, 0.0f), sunDir, 0.05f, 15.0f, 8.0f);

    // Blend water color and reflection by Fresnel
    float3 color = lerp(waterColor, reflColor, fresnel);

    // Add foam layer
    color = lerp(color, float3(0.92f, 0.96f, 1.0f), foam * 0.7f);

    // Specular on top (not shadowed for now — specular can be direct)
    color += u_sunColor * spec * 3.5f;

    // Multiply non-foam by shadow (diffuse sun contribution)
    float sunDiff = max(0.0f, dot(N, sunDir)) * shadow * u_sunI * (1.0f - foam);
    color += u_sunColor * sunDiff * waterColor * 0.12f;

    return color;
}

// ---------------------------------------------------------------------------
// Solid object albedo
// ---------------------------------------------------------------------------
float3 solidAlbedo(float matId, float3 pos)
{
    if (matId < 1.5f) return float3(0.90f, 0.35f, 0.10f);  // sphere: warm orange
    if (matId < 2.5f) return float3(0.20f, 0.40f, 0.75f);  // box: steel blue
    return float3(0.22f, 0.52f, 0.18f);                      // capsule: mossy green
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
float4 PSMain(float4 svPos : SV_POSITION) : SV_TARGET
{
    // RayMarch camera mode uses explicit basis vectors to avoid far-world
    // subtraction. PBR camera mode falls back to the original invVP path.
    float2 uv  = svPos.xy / float2(u_renderW, u_renderH);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y      = -ndc.y;

    float3 ro = u_camPos;
    float3 rd;
    float3 right = u_extra1.xyz;
    float3 up = u_extra2.xyz;
    float3 forward = normalize(cross(right, up));
    if (u_extra0.w > 0.5f) {
        rd = normalize(forward + right * (ndc.x * u_extra0.y) + up * (ndc.y * u_extra0.z));
    } else {
        float4 near4  = mul(float4(ndc, 0.0f, 1.0f), u_invVP);
        float4 far4   = mul(float4(ndc, 1.0f, 1.0f), u_invVP);
        float3 nearWS = near4.xyz / near4.w;
        float3 farWS  = far4.xyz  / far4.w;
        rd = normalize(farWS - nearWS);
    }

    // Cone marching: compute pixel footprint half-angle so the termination
    // epsilon grows proportionally with distance (Quilez / Keinert et al.).
    // At distance t the cone radius is pixelConeAngle*t — we stop when the
    // SDF is smaller than that radius, matching screen-space quality at any t.
    float2 adjNdc         = float2(ndc.x, ndc.y + 2.0f / u_renderH);
    float3 adjRd;
    if (u_extra0.w > 0.5f) {
        adjRd = normalize(forward + right * (adjNdc.x * u_extra0.y) + up * (adjNdc.y * u_extra0.z));
    } else {
        float4 near4   = mul(float4(ndc, 0.0f, 1.0f), u_invVP);
        float4 adjFar4 = mul(float4(adjNdc, 1.0f, 1.0f), u_invVP);
        float3 nearWS  = near4.xyz / near4.w;
        adjRd = normalize(adjFar4.xyz / adjFar4.w - nearWS);
    }
    float  pixelConeAngle = max(length(rd - adjRd), 1e-5f);

    // ---- Ray march ----
    // camLocal is computed once: it is always small (< kFloatOriginCell) so
    // camLocal + rd.xz * t never suffers catastrophic cancellation even when
    // the camera world-XZ is ~10^6.  This is used to produce a precision-safe
    // water SDF that overrides the one inside map() (which goes through the
    // large sum ro.xz + rd.xz * t → MakeStableXZ → still cancels).
    float2 camLocal = CamLocalXZ();
    float t     = 0.02f;
    float matId = -1.0f;
    for (int i = 0; i < kMaxSteps; ++i) {
        float3 p = ro + rd * t;
        float2 h = map(p);
        // Precision-safe water SDF.
        // camLocal + rd.xz*t: both operands are small → no cancellation.
        // Override whenever map() chose water OR when precise water is closer
        // than whatever solid map() returned (guards against imprecise union).
        float2 stableXZ = camLocal + rd.xz * t;
        float  dWaterP  = (p.y - waveHeight(stableXZ, u_time)) * 0.75f;
        if (h.y < 0.5f || dWaterP < h.x) h = float2(dWaterP, 0.0f);
        bool  isWater = (h.y < 0.5f);
        float epsilon = isWater ? kEps : max(pixelConeAngle * t * 0.2f, kEps);
        if (h.x < epsilon) { matId = h.y; break; }
        t += h.x;
        if (t > kMaxDist) break;
    }

    // ---- Miss: procedural sky + volumetric clouds ----
    if (matId < 0.0f) {
        // Debug heatmap: sky = white (no hit distance)
        if (u_extra0.x > 0.5f) return float4(1.0f, 1.0f, 1.0f, 1.0f);
        float3 sunDir = normalize(u_sunDir);
        float3 col = ComputeSkyColor(rd, sunDir, u_sunColor, u_sunI);

        if (rd.y > 0.01f && u_cloudCover > 0.01f) {
            float cloudDensity = max(u_cloudDensity, 0.5f);
            // Per-pixel jitter in [0,1): breaks fixed-step banding (horizontal lines)
            // at cloud edges where transmittance changes rapidly.
            float jitter = frac(sin(dot(svPos.xy, float2(127.1f, 311.7f))) * 43758.5453f);
            float4 clouds = MarchClouds(rd, sunDir, u_sunColor, u_sunI,
                                        u_time, u_cloudCover, cloudDensity, jitter);
            col = lerp(col, clouds.rgb / max(clouds.a, 1e-4f), saturate(clouds.a));
        }

        return float4(col, 1.0f);
    }

    float3 pos = ro + rd * t;

    // ---- Water surface ----
    float3 color;
    if (matId < 0.5f) {
        color = shadeWater(pos, rd, t);
    }
    else {
        // ---- Solid objects ----
        float3 N  = calcNormal(pos);
        float3 V  = -rd;
        float3 sunDir = normalize(u_sunDir);

        float ao     = calcAO(pos, N);
        float shadow = softShadow(pos + N * 0.002f, sunDir, 0.01f, 20.0f, 8.0f);

        float3 albedo = solidAlbedo(matId, pos);

        float NdotL   = max(0.0f, dot(N, sunDir));
        float3 diffuse = albedo * u_sunColor * u_sunI * NdotL * shadow;
        float3 ambient = albedo * skyColor(N) * 0.15f * ao;

        float3 H      = normalize(sunDir + V);
        float3 spec   = u_sunColor * u_sunI * pow(max(0.0f, dot(N, H)), 48.0f) * shadow * 0.3f;

        color = diffuse + ambient + spec;
    }

    // Atmospheric fog
    float fogFactor = 1.0f - exp(-t * 0.007f);
    color = lerp(color, skyColor(rd) * 0.55f, fogFactor);

    // ── DEBUG: Distance Heatmap ──────────────────────────────────────────────
    // u_extra0.x > 0.5 → override output with hit-distance color.
    // blue(near) → cyan → green → yellow → red(kMaxDist)
    // Sky pixels are rendered white so they don't confuse the distance reading.
    if (u_extra0.x > 0.5f)
    {
        float tn = saturate(t / kMaxDist);
        float3 dbgCol;
        if      (tn < 0.25f) { float s = tn / 0.25f;          dbgCol = lerp(float3(0,0,1), float3(0,1,1), s); }
        else if (tn < 0.50f) { float s = (tn-0.25f) / 0.25f;  dbgCol = lerp(float3(0,1,1), float3(0,1,0), s); }
        else if (tn < 0.75f) { float s = (tn-0.50f) / 0.25f;  dbgCol = lerp(float3(0,1,0), float3(1,1,0), s); }
        else                 { float s = (tn-0.75f) / 0.25f;  dbgCol = lerp(float3(1,1,0), float3(1,0,0), s); }
        return float4(dbgCol, 1.0f);
    }

    return float4(color, 1.0f);
}
