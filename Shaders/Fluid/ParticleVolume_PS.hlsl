// Ray-marched particle volume (wind fire + smoke), ParticleVolumeRenderPass.
// Cell float4 = (smokeDensity, soot 0..1, flameDensity, flameAge 0..1), splatted on the CPU by
// ParticleSplatGrid. Smoke scatters sunlight (Beer-Lambert, short self-shadow march, Henyey-Greenstein
// phase as in SmokeVolume_PS). Flames emit blackbody light coloured by temperature (Nguyen, Fedkiw &
// Jensen 2002) using the same Tanner Helland fit as FlameColor.cpp. Every step integrates its constant
// source analytically: (S - S e^(-sigma_t d)) / sigma_t (Hillaire 2015, "Physically Based and Unified
// Volumetric Rendering in Frostbite", slide 28).

cbuffer ParticleVolumeCB : register(b0)
{
    row_major float4x4 u_invViewProj;
    row_major float4x4 u_viewProj;
    float4             u_camPos;   // xyz = camera position
    float4             u_extra1;   // xyz = light direction (light to surface), w = intensity
    float4             u_extra2;   // rgb = light colour
    float4             u_extra3;   // xyz = grid origin, w = cell size
    float4             u_extra4;   // xyz = dims
    float4             u_extra5;   // x = smoke sigma_t, y = flame sigma_t, z = flame emission, w = soot darkening
};

StructuredBuffer<float4> g_cells : register(t0);

Texture2D<float> SceneDepthTex : register(t1);
SamplerState DepthSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

static const float PI = 3.14159265f;

float4 FetchCell(int3 idx, int3 dims)
{
    if (any(idx < 0) || any(idx >= dims)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);  // empty outside, so the volume fades at the grid edge
    }
    const uint i = ((uint)idx.z * (uint)dims.y + (uint)idx.y) * (uint)dims.x + (uint)idx.x;
    return g_cells[i];
}

float4 SampleCells(float3 worldPos, int3 dims)
{
    const float3 c = (worldPos - u_extra3.xyz) / u_extra3.w - 0.5f;
    const int3 i0 = int3(floor(c));
    const float3 f = c - float3(i0);
    const float4 c000 = FetchCell(i0 + int3(0, 0, 0), dims);
    const float4 c100 = FetchCell(i0 + int3(1, 0, 0), dims);
    const float4 c010 = FetchCell(i0 + int3(0, 1, 0), dims);
    const float4 c110 = FetchCell(i0 + int3(1, 1, 0), dims);
    const float4 c001 = FetchCell(i0 + int3(0, 0, 1), dims);
    const float4 c101 = FetchCell(i0 + int3(1, 0, 1), dims);
    const float4 c011 = FetchCell(i0 + int3(0, 1, 1), dims);
    const float4 c111 = FetchCell(i0 + int3(1, 1, 1), dims);
    const float4 c0 = lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y);
    const float4 c1 = lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y);
    return lerp(c0, c1, f.z);
}

float PhaseHG(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * PI * max(denom * sqrt(max(denom, 1e-6f)), 1e-6f));
}

// Tanner Helland's blackbody fit, 0..1 sRGB (FlameColor.cpp BlackbodySrgb / 255).
float3 BlackbodySrgb(float kelvin)
{
    const float t = clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * log(t) - 161.1195681661f;
    } else {
        r = 329.698727446f * pow(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * pow(t - 60.0f, -0.0755148492f);
    }
    if (t >= 66.0f) {
        b = 255.0f;
    } else if (t <= 19.0f) {
        b = 0.0f;
    } else {
        b = 138.5177312231f * log(t - 10.0f) - 305.0447927307f;
    }
    return saturate(float3(r, g, b) / 255.0f);
}

// FlameColor.cpp FlameTemperature: 2600 K at the root (age 0) cooling to 1000 K at the tip (age 1).
float FlameTemperature(float age01)
{
    const float a = saturate(age01);
    const float cool = 1.0f - (1.0f - a) * (1.0f - a);
    return 2600.0f + (1000.0f - 2600.0f) * cool;
}

bool IntersectBox(float3 ro, float3 rd, float3 bmin, float3 bmax, out float tNear, out float tFar)
{
    const float3 inv = 1.0f / rd;
    const float3 t0 = (bmin - ro) * inv;
    const float3 t1 = (bmax - ro) * inv;
    const float3 tmin = min(t0, t1);
    const float3 tmax = max(t0, t1);
    tNear = max(max(tmin.x, tmin.y), tmin.z);
    tFar = min(min(tmax.x, tmax.y), tmax.z);
    return tFar > max(tNear, 0.0f);
}

// Jimenez 2014 interleaved gradient noise: per-pixel start offset that trades banding for fine noise.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float4 PSMain(PSInput input) : SV_Target0
{
    const float2 ndcXY = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    const float4 nearH = mul(float4(ndcXY, 1.0f, 1.0f), u_invViewProj);
    const float4 farH = mul(float4(ndcXY, 0.0f, 1.0f), u_invViewProj);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float3 rayDir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
    const float3 ro = u_camPos.xyz;

    const int3 dims = int3(u_extra4.xyz + 0.5f);
    const float cell = u_extra3.w;
    const float3 bmin = u_extra3.xyz;
    const float3 bmax = bmin + float3(dims) * cell;
    float tNear, tFar;
    if (!IntersectBox(ro, rayDir, bmin, bmax, tNear, tFar)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    tNear = max(tNear, 0.0f);

    const float sceneDepth = SceneDepthTex.SampleLevel(DepthSampler, input.uv, 0);
    if (sceneDepth > 0.0f) {  // reverse-Z: 0 = sky
        const float4 sceneH = mul(float4(ndcXY, sceneDepth, 1.0f), u_invViewProj);
        if (abs(sceneH.w) > 1e-7f) {
            tFar = min(tFar, dot(sceneH.xyz / sceneH.w - ro, rayDir));
        }
    }
    if (tFar <= tNear) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Half a cell per step (the splat kernel spans several cells), bounded for cost.
    const float span = tFar - tNear;
    const int steps = clamp((int)ceil(span / (0.5f * cell)), 8, 128);
    const float dt = span / (float)steps;
    const float jitter = InterleavedGradientNoise(input.position.xy);

    const float smokeSigma = u_extra5.x;
    const float flameSigma = u_extra5.y;
    const float flameEmission = u_extra5.z;
    const float sootDarkening = u_extra5.w;

    const float3 toLight = normalize(-u_extra1.xyz);
    const float3 sun = u_extra2.rgb * u_extra1.w;
    const float cosTheta = dot(rayDir, toLight);
    const float3 ambient = float3(0.34f, 0.38f, 0.44f);
    const int shadowSteps = 4;
    const float shadowStep = 1.5f * cell;

    float transmittance = 1.0f;
    float3 accum = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (int s = 0; s < steps; ++s) {
        const float t = tNear + ((float)s + jitter) * dt;
        const float3 p = ro + rayDir * t;
        const float4 c = SampleCells(p, dims);
        const float smoke = max(c.x, 0.0f);
        const float flame = max(c.z, 0.0f);
        const float sigmaT = smoke * smokeSigma + flame * flameSigma;
        if (sigmaT < 1e-5f) {
            continue;
        }

        float3 source = float3(0.0f, 0.0f, 0.0f);
        if (smoke > 1e-4f) {
            // Self-shadow toward the light through smoke and flames.
            float shadowDepth = 0.0f;
            [loop]
            for (int k = 1; k <= shadowSteps; ++k) {
                const float4 sc = SampleCells(p + toLight * (shadowStep * (float)k), dims);
                shadowDepth += (max(sc.x, 0.0f) * smokeSigma + max(sc.z, 0.0f) * flameSigma) * shadowStep;
            }
            // Multiple scattering, Wrenninge 2013 octaves (also Hillaire 2016 "Physically Based Sky,
            // Atmosphere and Cloud Rendering in Frostbite"): octave o scales the light by a^o, the optical
            // depth by b^o and the phase eccentricity by c^o, a = b = c = 0.5. Without it the shadowed
            // core of a plume stays as dark as the ambient term while its rim glows.
            float sunLight = 0.0f;
            float weight = 1.0f;
            float depthScale = 1.0f;
            float eccentricity = 1.0f;
            [unroll]
            for (int o = 0; o < 3; ++o) {
                const float octavePhase = lerp(PhaseHG(cosTheta, 0.55f * eccentricity),
                                               PhaseHG(cosTheta, -0.2f * eccentricity), 0.35f);
                sunLight += weight * exp(-shadowDepth * depthScale) * octavePhase;
                weight *= 0.5f;
                depthScale *= 0.5f;
                eccentricity *= 0.5f;
            }
            const float soot = saturate(c.y);
            const float3 albedo = float3(0.62f, 0.62f, 0.65f) * (1.0f - sootDarkening * soot);
            // Scattering coefficient = albedo * smoke sigma_t; the source term is radiance per unit sigma_t.
            source += (smoke * smokeSigma) * albedo * (sun * sunLight * 4.0f + ambient);
        }
        if (flame > 1e-4f) {
            const float age = saturate(c.w);
            const float3 srgb = BlackbodySrgb(FlameTemperature(age));
            const float3 linearColor = srgb * srgb;  // ~ sRGB to linear (gamma 2)
            const float heat = lerp(1.0f, 0.3f, age);
            source += flame * flameEmission * heat * linearColor;
        }

        // Hillaire 2015: integrate S over the step with the transmittance falloff inside it.
        const float stepT = exp(-sigmaT * dt);
        accum += transmittance * (source - source * stepT) / sigmaT;
        transmittance *= stepT;
        if (transmittance < 0.003f) {
            break;
        }
    }

    const float alpha = 1.0f - transmittance;
    const float peak = max(accum.r, max(accum.g, accum.b));
    if (peak < 1e-5f && alpha < 1e-5f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // The blend is SRC_ALPHA / INV_SRC_ALPHA, so hand back accum / alpha. Emission can exceed the
    // alpha it comes with; the alpha floor keeps accum / alpha inside fp16 and darkens the scene
    // behind by at most that floor.
    const float outAlpha = max(alpha, max(1e-3f, peak / 60000.0f));
    return float4(accum / outAlpha, outAlpha);
}
