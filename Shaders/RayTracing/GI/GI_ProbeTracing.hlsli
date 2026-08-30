#ifndef SASAMI_GI_PROBE_TRACING_HLSLI
#define SASAMI_GI_PROBE_TRACING_HLSLI

// SH projection, probe ray distribution, and hit shading helpers.

void ProjectOntoSH(float3 dir, float3 radiance, inout float3 sh[kSHCount])
{
    float x = dir.x, y = dir.y, z = dir.z;
    sh[0] += radiance * 0.282095f;
    sh[1] += radiance * (0.488603f * y);
    sh[2] += radiance * (0.488603f * z);
    sh[3] += radiance * (0.488603f * x);
    sh[4] += radiance * (1.092548f * x * y);
    sh[5] += radiance * (1.092548f * y * z);
    sh[6] += radiance * (0.315392f * (3.0f * z * z - 1.0f));
    sh[7] += radiance * (1.092548f * x * z);
    sh[8] += radiance * (0.546274f * (x * x - y * y));
}

// Scalar overload: projects a scalar field (e.g. hit distance) onto the same
// L2 SH basis. Used to bake a per-probe directional visibility signal into
// the otherwise-unused .w channel of the probe SH buffer. Same raw basis
// constants as the float3 overload above — deliberately NOT the cosine-lobe
// convolved constants used by GI_EvaluateProbeSH (those are irradiance-specific).
void ProjectOntoSH(float3 dir, float value, inout float sh[kSHCount])
{
    float x = dir.x, y = dir.y, z = dir.z;
    sh[0] += value * 0.282095f;
    sh[1] += value * (0.488603f * y);
    sh[2] += value * (0.488603f * z);
    sh[3] += value * (0.488603f * x);
    sh[4] += value * (1.092548f * x * y);
    sh[5] += value * (1.092548f * y * z);
    sh[6] += value * (0.315392f * (3.0f * z * z - 1.0f));
    sh[7] += value * (1.092548f * x * z);
    sh[8] += value * (0.546274f * (x * x - y * y));
}

// --------------------------------------------------------------------------
// Direction sampling 窶・Fibonacci lattice on unit sphere
// Produces uniform, low-discrepancy sample directions.
// jitter (0-1) offsets the sequence for temporal decorrelation.
// --------------------------------------------------------------------------
float3 FibonacciSphereDir(uint i, uint n, float2 jitter)
{
    const float kGoldenAngle = 2.39996323f;
    float fi    = float(i) + frac(jitter.x);
    float theta = acos(clamp(1.0f - 2.0f * fi / float(n), -1.0f, 1.0f));
    float phi   = kGoldenAngle * (fi + jitter.y * float(n));
    float sinT  = sin(theta);
    return normalize(float3(sinT * cos(phi), cos(theta), sinT * sin(phi)));
}

// --------------------------------------------------------------------------
// PBR shade at hit point: NEE against the directional light, all punctual
// point/spot lights (g_pointLights/g_spotLights, same GpuPointLightRT/
// GpuSpotLightRT layout and falloff as SWRT_Reflection_Shading.hlsli's
// ShadeDirectPBR), plus a single-sample procedural-sky ambient term.
// No IBL prefiltered-cubemap / SH-irradiance sky term here: that texture is
// only reachable through the frame-graph wiring that assembles UpdateDesc,
// which is outside this GI-only file scope.
// --------------------------------------------------------------------------

float3 FresnelSchlickGI(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float GGX_D_GI(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * d * d, 1e-7f);
}

float GGX_V_GI(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gL = NdotL / max(NdotL * (1.0f - k) + k, 1e-5f);
    float gV = NdotV / max(NdotV * (1.0f - k) + k, 1e-5f);
    return gL * gV;
}

float3 ShadePBRAtHit(float3 pos, float3 N, float3 V, GpuMaterial mat)
{
    float roughness = saturate(mat.roughness);
    float3 F0 = SWRT_MaterialF0(mat);
    float3 diffuseReflectance = SWRT_MaterialDiffuseReflectance(mat);
    float3 outColor = max(mat.emissive, 0.0f);

    // Directional light NEE
    float3 L = normalize(g_dirLightDir);
    float NdotL = max(dot(N, L), 0.0f);
    if (NdotL > 0.0f)
    {
        bool inShadow = TraceAnyHit(pos + N * g_shadowBias, L, 0.001f, 200.0f);
        if (!inShadow)
        {
            float3 H     = normalize(L + V);
            float NdotV  = max(dot(N, V), 0.001f);
            float NdotH  = saturate(dot(N, H));
            float VdotH  = saturate(dot(V, H));
            float3 F     = FresnelSchlickGI(VdotH, F0);
            float  D     = GGX_D_GI(NdotH, max(roughness, 0.05f));
            float  Vis   = GGX_V_GI(NdotL, NdotV, max(roughness, 0.05f));
            float3 spec  = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
            float3 kd    = (1.0f - F);
            float3 diff  = kd * diffuseReflectance / 3.14159265f;
            outColor    += (diff + spec) * NdotL * g_dirLightColor * g_dirLightIntensity;
        }
    }

    // Point lights NEE (same falloff/BRDF as SWRT_Reflection_Shading.hlsli's ShadeDirectPBR,
    // ray-traced shadows instead of shadow maps since this runs inside the SWRT BVH pass).
    [loop]
    for (uint pi = 0; pi < g_pointLightCount; ++pi)
    {
        GpuPointLightRT pl = g_pointLights[pi];
        float3 toLight = pl.pos - pos;
        float dist = length(toLight);
        if (dist <= 1e-4f || dist >= pl.range) continue;

        float3 Lp = toLight / dist;
        float NdotLp = max(dot(N, Lp), 0.0f);
        if (NdotLp <= 0.0f) continue;

        bool inShadow = TraceAnyHit(pos + N * g_shadowBias, Lp, 0.001f, dist - 0.001f);
        if (inShadow) continue;

        float t = dist / max(pl.range, 1e-4f);
        float atten = saturate(1.0f - t * t);
        atten = atten * atten;

        float3 H    = normalize(Lp + V);
        float NdotV = max(dot(N, V), 0.001f);
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float3 F    = FresnelSchlickGI(VdotH, F0);
        float  D    = GGX_D_GI(NdotH, max(roughness, 0.05f));
        float  Vis  = GGX_V_GI(NdotLp, NdotV, max(roughness, 0.05f));
        float3 spec = (F * D * Vis) / max(4.0f * NdotLp * NdotV, 0.001f);
        float3 kd   = (1.0f - F);
        float3 diff = kd * diffuseReflectance / 3.14159265f;
        outColor   += (diff + spec) * NdotLp * pl.colorIntensity * atten;
    }

    // Spot lights NEE
    [loop]
    for (uint si = 0; si < g_spotLightCount; ++si)
    {
        GpuSpotLightRT sl = g_spotLights[si];
        float3 toLight = sl.pos - pos;
        float dist = length(toLight);
        if (dist <= 1e-4f || dist >= sl.range) continue;

        float3 Ls = toLight / dist;
        float cosA = dot(-Ls, normalize(sl.dir));
        if (cosA < sl.cosOuter) continue;

        float NdotLs = max(dot(N, Ls), 0.0f);
        if (NdotLs <= 0.0f) continue;

        bool inShadow = TraceAnyHit(pos + N * g_shadowBias, Ls, 0.001f, dist - 0.001f);
        if (inShadow) continue;

        float spotAtten = smoothstep(sl.cosOuter, sl.cosInner, cosA);
        float t = dist / max(sl.range, 1e-4f);
        float atten = saturate(1.0f - t * t);
        atten = atten * atten * spotAtten;

        float3 H    = normalize(Ls + V);
        float NdotV = max(dot(N, V), 0.001f);
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float3 F    = FresnelSchlickGI(VdotH, F0);
        float  D    = GGX_D_GI(NdotH, max(roughness, 0.05f));
        float  Vis  = GGX_V_GI(NdotLs, NdotV, max(roughness, 0.05f));
        float3 spec = (F * D * Vis) / max(4.0f * NdotLs * NdotV, 0.001f);
        float3 kd   = (1.0f - F);
        float3 diff = kd * diffuseReflectance / 3.14159265f;
        outColor   += (diff + spec) * NdotLs * sl.colorIntensity * atten;
    }

    // Sky ambient: single-sample sky probe along the surface normal, standing in for the
    // diffuse-irradiance hemispherical integral (no SH/prefiltered-cubemap IBL reachable
    // from this GI-only file scope -- see file header). Same procedural sky model as the
    // scene's environment miss shading above, so a probe's ambient stays consistent with
    // what a probe ray that missed the scene would have recorded. Not divided by PI: this
    // mirrors the flat-ambient convention it replaces (treated as an already-integrated
    // reflected-ambient term, not as an irradiance needing the Lambert 1/PI -- that
    // normalization is applied once, downstream, where GI_SampleProbeGrid's output is
    // consumed in CookTorranceGGX_PS.hlsl / DeferredLighting_PS.hlsl).
    // Flat ambient, NOT a sky-radiance lookup. Sampling ComputeSkyColor() here blew the
    // whole image out: this runs at every ray HIT point, where the probe ray's own bounce
    // already carries the indirect light, and it has no visibility term -- so every hit,
    // including deep interiors, gained unoccluded full sky radiance on top. Sky radiance
    // also dwarfs the 0.1 constant this replaced. The sky belongs on the MISS path (see
    // GI_ProbeUpdate_CS.hlsl), where the ray genuinely sees it; here it is double counting.
    outColor += diffuseReflectance * g_ambientColor * g_ambientIntensity;
    return outColor;
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

#endif // SASAMI_GI_PROBE_TRACING_HLSLI
