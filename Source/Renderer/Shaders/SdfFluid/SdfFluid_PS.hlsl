// SdfFluid_PS.hlsl
// Full-frame SDF renderer: procedural sky + liquid / smoke / fire / combined fluid.
//
// CB layout matches CameraCBData (mvp + world + extra0/1/2) so PushCameraCB can be reused:
//   mvp   [16] -> u_invVP        (camera inverse view-projection, row-major)
//   world [ 0.. 3] -> camPos.xyz, time
//   world [ 4.. 7] -> sunDir.xyz, sunIntensity
//   world [ 8..11] -> sunColor.rgb, cloudCover (sky)
//   world [12..15] -> renderW, renderH, fluidMode (0=liquid 1=smoke 2=fire 3=combined), pad
//   extra0 -> fluidCenter.xyz, fluidRadius
//   extra1 -> fluidColor.rgb,  fluidDensity
//   extra2 -> fluidSpeed, fluidDetail, fluidRoughness, fluidIOR

cbuffer SdfFluidCB : register(b0)
{
    row_major float4x4 u_invVP;

    float3 u_camPos;   float u_time;
    float3 u_sunDir;   float u_sunI;
    float3 u_sunColor; float u_cloudCover;
    float  u_renderW;  float u_renderH; float u_modeF; float u_pad0;

    float3 u_fCenter;  float u_fRadius;
    float3 u_fColor;   float u_fDensity;
    float  u_fSpeed;   float u_fDetail; float u_fRough; float u_fIOR;
}

static const float PI      = 3.14159265358979f;
static const float kEps    = 0.001f;
static const int   kMaxSteps = 128;
static const float kMaxDist  = 300.0f;

// ---------------------------------------------------------------------------
// Helpers: noise / FBM
// ---------------------------------------------------------------------------
float Hash3(float3 p)
{
    p  = frac(p * float3(443.8975f, 441.423f, 437.195f));
    p += dot(p, p.yxz + 19.19f);
    return frac((p.x + p.y) * p.z);
}

float SmoothNoise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    return lerp(
        lerp(lerp(Hash3(i),                  Hash3(i + float3(1,0,0)), f.x),
             lerp(Hash3(i + float3(0,1,0)),  Hash3(i + float3(1,1,0)), f.x), f.y),
        lerp(lerp(Hash3(i + float3(0,0,1)),  Hash3(i + float3(1,0,1)), f.x),
             lerp(Hash3(i + float3(0,1,1)),  Hash3(i + float3(1,1,1)), f.x), f.y), f.z);
}

float FBM5(float3 p)
{
    float v = 0.0f, a = 0.5f;
    [unroll]
    for (int i = 0; i < 5; i++) {
        v  += a * SmoothNoise(p);
        p   = p * 2.02f + float3(73.1f, 61.4f, 53.7f);
        a  *= 0.5f;
    }
    return v;
}

// ---------------------------------------------------------------------------
// SDF primitives
// ---------------------------------------------------------------------------
float sdSphere(float3 p, float r) { return length(p) - r; }

float sdGround(float3 p) { return p.y + 2.5f; } // ground at y = -2.5

// FBM-deformed fluid blob centred at u_fCenter
float sdFluid(float3 p)
{
    float3 lp  = p - u_fCenter;
    float  d   = sdSphere(lp, u_fRadius);
    // Surface wave deformation via FBM on sphere normal
    float3 np  = normalize(lp) * u_fDetail;
    np.xz += u_time * u_fSpeed;
    float wave = (FBM5(np) * 2.0f - 1.0f) * 0.18f * u_fRadius;
    return d - wave;
}

// Smooth minimum (polynomial blend, k = blend radius)
float SMin(float a, float b, float k)
{
    float h = saturate(0.5f + 0.5f * (b - a) / k);
    return lerp(b, a, h) - k * h * (1.0f - h);
}

// Scene SDF — returns (dist, materialID): 1=fluid, 2=ground
float2 SceneSDF(float3 p)
{
    float dF = sdFluid(p);
    float dG = sdGround(p);
    if (dF < dG) return float2(dF, 1.0f);
    return float2(dG, 2.0f);
}

float3 SceneNormal(float3 p)
{
    const float h = 0.0005f;
    return normalize(float3(
        SceneSDF(p + float3(h,0,0)).x - SceneSDF(p - float3(h,0,0)).x,
        SceneSDF(p + float3(0,h,0)).x - SceneSDF(p - float3(0,h,0)).x,
        SceneSDF(p + float3(0,0,h)).x - SceneSDF(p - float3(0,0,h)).x
    ));
}

// ---------------------------------------------------------------------------
// Ray march (sphere-stepping)
// ---------------------------------------------------------------------------
float2 RayMarch(float3 ro, float3 rd)
{
    float t = 0.05f;
    for (int i = 0; i < kMaxSteps; i++) {
        float2 sd = SceneSDF(ro + rd * t);
        if (sd.x < kEps) return float2(t, sd.y);
        t += sd.x;
        if (t > kMaxDist) break;
    }
    return float2(-1.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Procedural sky (Rayleigh + Mie, reused from ProceduralSky)
// ---------------------------------------------------------------------------
float3 SkyColor(float3 rd)
{
    float3 sunDir = normalize(u_sunDir);
    float  cosT   = dot(rd, sunDir);
    float  elev   = rd.y;

    float3 zenith  = float3(0.05f, 0.15f, 0.65f) * 2.0f;
    float3 horizon = float3(0.50f, 0.68f, 0.88f) * 1.5f;
    float3 ground  = float3(0.08f, 0.06f, 0.04f);

    float3 skyBase;
    if (elev >= 0.0f)
        skyBase = lerp(horizon, zenith, pow(saturate(elev), 0.4f));
    else
        skyBase = lerp(horizon, ground, saturate(-elev * 5.0f));

    float sunInfl = saturate(cosT * 0.5f + 0.5f);
    skyBase *= lerp(1.0f, u_sunColor * float3(1.1f, 0.9f, 0.7f), sunInfl * 0.4f);

    float mie  = pow(max(cosT, 0.0f), 6.0f) * 0.3f;
    float3 glow = u_sunColor * pow(saturate(1.0f - abs(elev)), 3.0f)
                * max(cosT, 0.0f) * u_sunI * 0.4f;

    float3 sky  = skyBase * u_sunI + u_sunColor * mie * u_sunI + glow;
    float  disc = smoothstep(0.9996f, 1.0f, cosT);
    sky += u_sunColor * disc * 12.0f * u_sunI;
    return max(sky, 0.0f);
}

// ---------------------------------------------------------------------------
// Cloud ray march (reused, simplified)
// ---------------------------------------------------------------------------
float CloudDensityAt(float3 pos)
{
    float3 p = pos * 0.0001f;
    p.x += u_time * 0.008f;
    p.z += u_time * 0.003f;
    float base   = FBM5(p * 0.8f);
    float detail = FBM5(p * 2.5f + float3(1.4f, 2.1f, 0.9f)) * 0.35f;
    return max(base + detail - (1.0f - u_cloudCover), 0.0f);
}

float4 MarchClouds(float3 ro, float3 rd)
{
    if (rd.y < 0.005f || u_cloudCover < 0.01f) return float4(0,0,0,0);
    float tBase = 1500.0f / rd.y;
    float tTop  = 5000.0f / rd.y;
    if (tBase > 300000.0f) return float4(0,0,0,0);
    const int kSteps = 32;
    float dt = (tTop - tBase) / float(kSteps);
    float g   = 0.6f, g2 = g * g;
    float cosS = dot(rd, normalize(u_sunDir));
    float denom = max(1.0f + g2 - 2.0f * g * cosS, 1e-6f);
    float phaseHG = (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));
    float trans = 1.0f;
    float3 scat = 0.0f;
    for (int i = 0; i < kSteps; i++) {
        if (trans < 0.02f) break;
        float  t    = tBase + (i + 0.5f) * dt;
        float3 pos  = ro + rd * t;
        float  dens = CloudDensityAt(pos);
        if (dens < 1e-4f) continue;
        float sigma = dens * 0.0008f;
        float sT    = exp(-sigma * dt);
        float3 sunLit = u_sunColor * u_sunI * exp(-dens * 1.2f) * phaseHG;
        float3 ambient = float3(0.35f, 0.50f, 0.72f) * 0.28f;
        scat  += (sunLit + ambient) * dens * trans * (1.0f - sT) / max(sigma, 1e-6f);
        trans *= sT;
    }
    return float4(scat, 1.0f - trans);
}

// ---------------------------------------------------------------------------
// Liquid: Fresnel + refraction (single bounce) + Beer-Lambert absorption
// ---------------------------------------------------------------------------
float3 TraceRefraction(float3 hitPos, float3 refrDir)
{
    // March inside fluid with fixed small steps
    float pathLen = 0.0f;
    float3 p = hitPos + refrDir * kEps * 4.0f;
    [loop]
    for (int i = 0; i < 48; i++) {
        float d = SceneSDF(p).x;
        if (d > 0.0f) break; // exited fluid
        float step = max(abs(d), 0.02f * u_fRadius);
        p       += refrDir * step;
        pathLen += step;
        if (pathLen > 4.0f * u_fRadius) break;
    }
    // Beer-Lambert: absorption = exp(-absorptionCoeff * pathLen)
    // Use fColor as absorption coefficient (saturated, per-channel)
    float3 absorption = exp(-saturate(u_fColor) * pathLen * 0.8f);
    // Exit normal & secondary refraction (air direction)
    float3 exitN   = SceneNormal(p);
    float3 exitDir = refract(refrDir, -exitN, u_fIOR);
    if (!any(exitDir != 0.0f)) exitDir = reflect(refrDir, -exitN); // TIR fallback
    float3 bgColor = SkyColor(exitDir);
    bgColor = bgColor / (bgColor + 1.0f); // tone map before absorption
    return bgColor * absorption;
}

float3 ShadeLiquid(float3 hitPos, float3 N, float3 rd)
{
    float3 V     = -rd;
    float  cosNV = max(dot(N, V), 0.0f);
    // Schlick Fresnel (water F0 ≈ 0.02)
    float F0      = 0.02f;
    float fresnel = F0 + (1.0f - F0) * pow(1.0f - cosNV, 5.0f);

    // Reflection
    float3 reflDir   = reflect(rd, N);
    float3 reflColor = SkyColor(reflDir);
    reflColor = reflColor / (reflColor + 1.0f);

    // Refraction
    float3 refrDir = refract(rd, N, 1.0f / u_fIOR);
    float3 refrColor;
    if (any(refrDir != 0.0f))
        refrColor = TraceRefraction(hitPos, refrDir);
    else {
        refrColor = reflColor; // total internal reflection
        fresnel   = 1.0f;
    }

    // Specular highlight (directional light)
    float3 H      = normalize(V + normalize(u_sunDir));
    float  spec   = pow(max(dot(N, H), 0.0f), lerp(4.0f, 256.0f, 1.0f - u_fRough));
    float3 specC  = u_sunColor * u_sunI * spec * fresnel;

    return lerp(refrColor, reflColor, fresnel) + specC;
}

// ---------------------------------------------------------------------------
// Ground shading (checkerboard diffuse)
// ---------------------------------------------------------------------------
float3 ShadeGround(float3 hitPos, float3 N)
{
    float3 albedo = float3(0.65f, 0.55f, 0.42f);
    float2 checker = floor(hitPos.xz * 0.5f);
    if (frac(checker.x + checker.y) < 0.5f) albedo *= 0.7f;

    float3 sunDir = normalize(u_sunDir);
    float  ndotl  = max(dot(N, sunDir), 0.0f);
    float3 ambient = albedo * float3(0.3f, 0.4f, 0.6f) * 0.35f;
    return albedo * u_sunColor * u_sunI * ndotl + ambient;
}

// ---------------------------------------------------------------------------
// Volumetric smoke / fire
// ---------------------------------------------------------------------------
float3 FireColor(float t)
{
    t = saturate(t);
    if (t < 0.4f) return lerp(float3(0,0,0), float3(1.0f,0.15f,0.0f), t / 0.4f);
    if (t < 0.7f) return lerp(float3(1.0f,0.15f,0.0f), float3(1.0f,0.65f,0.0f), (t-0.4f)/0.3f);
    return lerp(float3(1.0f,0.65f,0.0f), float3(1.0f,1.0f,0.7f), (t-0.7f)/0.3f);
}

float VolumeDensityAt(float3 p)
{
    float3 lp   = p - u_fCenter;
    float  d    = sdSphere(lp, u_fRadius * 1.3f);
    if (d > 0.0f) return 0.0f; // outside sphere bounds

    float3 np   = (lp / u_fRadius) * u_fDetail;
    np.y       -= u_time * u_fSpeed;   // rising upward
    np.x       += u_time * u_fSpeed * 0.2f;
    float dens = FBM5(np) * u_fDensity;
    return max(dens - 0.2f, 0.0f);
}

float4 MarchVolume(float3 ro, float3 rd)
{
    // Intersect sphere bounding volume
    float3 oc  = ro - u_fCenter;
    float  b   = dot(oc, rd);
    float  c   = dot(oc, oc) - (u_fRadius * 1.3f) * (u_fRadius * 1.3f);
    float  disc = b*b - c;
    if (disc < 0.0f) return float4(0,0,0,0);
    float sqrtD = sqrt(disc);
    float tMin  = max(-b - sqrtD, 0.0f);
    float tMax  = -b + sqrtD;
    if (tMax <= tMin) return float4(0,0,0,0);

    const int kSteps = 48;
    float dt = (tMax - tMin) / float(kSteps);

    float g   = 0.5f, g2 = g * g;
    float cosS = dot(rd, normalize(u_sunDir));
    float denom = max(1.0f + g2 - 2.0f * g * cosS, 1e-6f);
    float phaseHG = (1.0f - g2) / (4.0f * PI * pow(denom, 1.5f));

    float trans = 1.0f;
    float3 scat = 0.0f;

    [loop]
    for (int i = 0; i < kSteps; i++) {
        if (trans < 0.01f) break;
        float  t    = tMin + (i + 0.5f) * dt;
        float3 p    = ro + rd * t;
        float  dens = VolumeDensityAt(p);
        if (dens < 1e-4f) continue;

        float sigma = dens * 0.6f;
        float sT    = exp(-sigma * dt);

        float3 emissive;
        int mode = int(u_modeF + 0.5f);
        if (mode == 2) { // fire
            emissive = FireColor(dens) * 4.0f;
        } else { // smoke (mode 1 or 3)
            float sunAtten = exp(-dens * 2.0f);
            emissive = u_sunColor * u_sunI * sunAtten * phaseHG;
            emissive += float3(0.3f, 0.4f, 0.6f) * 0.25f; // ambient
        }

        scat  += emissive * dens * trans * (1.0f - sT) / max(sigma, 1e-6f);
        trans *= sT;
    }

    return float4(scat, 1.0f - trans);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
float4 PSMain(float4 svPos : SV_POSITION) : SV_TARGET
{
    // Reconstruct NDC pixel center
    float ndcX = (svPos.x / u_renderW) * 2.0f - 1.0f;
    float ndcY = -((svPos.y / u_renderH) * 2.0f - 1.0f); // flip Y

    // World-space ray via inverse VP
    float4 clipFar  = mul(float4(ndcX, ndcY, 1.0f, 1.0f), u_invVP);
    float4 clipNear = mul(float4(ndcX, ndcY, 0.0f, 1.0f), u_invVP);
    float3 wFar     = clipFar.xyz  / clipFar.w;
    float3 wNear    = clipNear.xyz / clipNear.w;
    float3 rd = normalize(wFar - wNear);
    float3 ro = u_camPos;

    int mode = int(u_modeF + 0.5f); // 0=liquid, 1=smoke, 2=fire, 3=combined

    // --- Volume (smoke / fire) ---
    float4 volume = float4(0,0,0,0);
    if (mode == 1 || mode == 2 || mode == 3) {
        volume = MarchVolume(ro, rd);
    }

    // --- SDF surface ray march ---
    float2 hit = RayMarch(ro, rd);
    float3 color;

    if (hit.x > 0.0f) {
        float3 hitPos = ro + rd * hit.x;
        float3 N      = SceneNormal(hitPos);
        int    matID  = int(hit.y + 0.5f);

        if (matID == 1 && (mode == 0 || mode == 3)) {
            color = ShadeLiquid(hitPos, N, rd);
        } else if (matID == 2) {
            color = ShadeGround(hitPos, N);
        } else {
            // Fluid surface, non-liquid mode: simple diffuse
            float  ndotl = max(dot(N, normalize(u_sunDir)), 0.0f);
            color = u_fColor * u_sunColor * u_sunI * ndotl + u_fColor * 0.08f;
        }
    } else {
        // Sky background
        float3 sky = SkyColor(rd);
        sky = sky / (sky + 1.0f); // Reinhard tone map
        // Cloud layer
        float4 clouds = MarchClouds(ro, rd);
        sky = lerp(sky, clouds.rgb / max(clouds.a, 1e-4f), saturate(clouds.a));
        color = sky;
    }

    // --- Composite volume over surface ---
    if ((mode == 1 || mode == 2 || mode == 3) && volume.a > 0.01f) {
        color = lerp(color, volume.rgb / max(volume.a, 1e-4f), saturate(volume.a));
    }

    // Gamma correction
    color = pow(max(color, 0.0f), 1.0f / 2.2f);
    return float4(color, 1.0f);
}
