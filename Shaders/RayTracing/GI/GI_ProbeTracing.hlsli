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
// Minimal PBR shade at hit point (NEE with directional light + ambient)
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

    // Sky ambient
    outColor += diffuseReflectance * g_ambientColor * g_ambientIntensity;
    return outColor;
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------

#endif // SASAMI_GI_PROBE_TRACING_HLSLI
