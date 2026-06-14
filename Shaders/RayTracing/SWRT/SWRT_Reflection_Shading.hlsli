#ifndef SASAMI_SWRT_REFLECTION_SHADING_HLSLI
#define SASAMI_SWRT_REFLECTION_SHADING_HLSLI

// Reflection sampling, direct-light shading, environment fallback, and transparent continuation helpers.

float3 ComputeCameraRayDir(uint2 pixel, uint2 resolution)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(resolution.x)) * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(resolution.y)) * 2.0f + 1.0f;
    float4 dir = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    return normalize(dir.xyz / dir.w - g_cameraPos);
}

// --------------------------------------------------------------------------
// Hammersley low-discrepancy sequence (deterministic, no temporal noise)
// --------------------------------------------------------------------------
float2 Hammersley(uint i, uint N)
{
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(N), float(bits) * 2.3283064365386963e-10f);
}

// GGX NDF importance sampling: returns half-vector in tangent space
float3 SampleGGX_H(float2 Xi, float roughness)
{
    float a   = roughness * roughness;
    float phi = 6.28318530718f * Xi.x;
    float cosT = sqrt((1.0f - Xi.y) / max(1.0f + (a*a - 1.0f) * Xi.y, 1e-7f));
    float sinT = sqrt(max(0.0f, 1.0f - cosT * cosT));
    return float3(sinT * cos(phi), sinT * sin(phi), cosT);
}

// Transform tangent-space vector to world space aligned with N
float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
}

// --------------------------------------------------------------------------
// Schlick fresnel
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    float f = pow(saturate(1.0f - cosTheta), 5.0f);
    return F0 + (1.0f - F0) * f;
}

// GGX NDF
float GGX_D(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}

// Smith GGX visibility (approximation)
float GGX_V(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    return gL * gV;
}

// Evaluate PBR (directional light only for reflection shading)
float3 ShadeDirectPBR(float3 pos, float3 N, float3 V, GpuMaterial mat)
{
    float roughness = saturate(mat.roughness);
    float3 F0 = SWRT_MaterialF0(mat);
    float3 diffuseReflectance = SWRT_MaterialDiffuseReflectance(mat);

    float3 outColor = max(mat.emissive, 0.0f);

    // ---- 逶ｴ謗･蜈・(NEE / MIS) ----------------------------------------
    // IS Only 繝｢繝ｼ繝峨・縺ｨ縺阪・繧ｹ繧ｭ繝・・縲・EE Only / MIS 縺ｧ縺ｯ蜈ｨ繝ｩ繧､繝医∈ shadow ray縲・
    if (g_samplingMode != SWRT_SAMPLE_IS)
    {
        // Directional light
        {
            float3 L     = normalize(g_dirLightDir);
            float  NdotL = max(dot(N, L), 0.0f);
            if (NdotL > 0.0f)
            {
                bool inShadow = TraceAnyHit(OffsetRay(pos, N), L, g_tMin, 200.0f);
                if (!inShadow)
                {
                    float3 H     = normalize(L + V);
                    float  NdotV = max(dot(N, V), 0.001f);
                    float  NdotH = saturate(dot(N, H));
                    float  VdotH = saturate(dot(V, H));

                    float3 F   = FresnelSchlick(VdotH, F0);
                    float  D   = GGX_D(NdotH, max(roughness, 0.05f));
                    float  Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));

                    float3 specular = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
                    float3 diffuse  = diffuseReflectance / 3.14159265f;
                    float3 kd = (1.0f - F);

                    outColor += (kd * diffuse + specular) * NdotL * g_dirLightColor * g_dirLightIntensity;
                }
            }
        }

        // Note: point/spot light NEE removed 窶・G-Buffer path uses directional light only.
        // The reflection hit point's local lights are evaluated via ShadePBR at the
        // secondary hit, which can be extended in future iterations.
        [loop]
        for (uint i = 0; i < g_pointLightCount; ++i)
        {
            GpuPointLightRT pl = g_pointLights[i];
            float3 toLight = pl.pos - pos;
            float dist = length(toLight);
            if (dist <= 1e-4f || dist >= pl.range) continue;

            float3 L = toLight / dist;
            float NdotL = max(dot(N, L), 0.0f);
            if (NdotL <= 0.0f) continue;

            bool inShadow = TraceAnyHit(OffsetRay(pos, N), L, g_tMin, dist - 0.001f);
            if (inShadow) continue;

            float t = dist / max(pl.range, 1e-4f);
            float atten = saturate(1.0f - t * t);
            atten *= atten;

            float3 H = normalize(L + V);
            float NdotV = max(dot(N, V), 0.001f);
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float3 F = FresnelSchlick(VdotH, F0);
            float D = GGX_D(NdotH, max(roughness, 0.05f));
            float Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));
            float3 specular = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
            float3 diffuse = diffuseReflectance / 3.14159265f;
            float3 kd = (1.0f - F);

            outColor += (kd * diffuse + specular) * NdotL * pl.colorIntensity * atten;
        }

        [loop]
        for (uint i = 0; i < g_spotLightCount; ++i)
        {
            GpuSpotLightRT sl = g_spotLights[i];
            float3 toLight = sl.pos - pos;
            float dist = length(toLight);
            if (dist <= 1e-4f || dist >= sl.range) continue;

            float3 L = toLight / dist;
            float cosA = dot(-L, normalize(sl.dir));
            if (cosA < sl.cosOuter) continue;

            float NdotL = max(dot(N, L), 0.0f);
            if (NdotL <= 0.0f) continue;

            bool inShadow = TraceAnyHit(OffsetRay(pos, N), L, g_tMin, dist - 0.001f);
            if (inShadow) continue;

            float spotAtten = smoothstep(sl.cosOuter, sl.cosInner, cosA);
            float t = dist / max(sl.range, 1e-4f);
            float atten = saturate(1.0f - t * t);
            atten = atten * atten * spotAtten;

            float3 H = normalize(L + V);
            float NdotV = max(dot(N, V), 0.001f);
            float NdotH = saturate(dot(N, H));
            float VdotH = saturate(dot(V, H));
            float3 F = FresnelSchlick(VdotH, F0);
            float D = GGX_D(NdotH, max(roughness, 0.05f));
            float Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));
            float3 specular = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
            float3 diffuse = diffuseReflectance / 3.14159265f;
            float3 kd = (1.0f - F);

            outColor += (kd * diffuse + specular) * NdotL * sl.colorIntensity * atten;
        }
    } // end NEE block

    return outColor;
}

float3 EvaluateReflectionHitAmbientFloor(GpuMaterial mat)
{
    // Small floor to avoid fully black traced hits indoors without injecting
    // sky color. Use the visible material color rather than diffuse-only color
    // so metallic reflection hits do not collapse to black.
    static const float kHitAmbientFloor = 0.06f;
    float3 diffuseColor = SWRT_MaterialDiffuseReflectance(mat);
    float3 visibleColor = lerp(diffuseColor, mat.baseColor.rgb, saturate(mat.metallic));
    return visibleColor * kHitAmbientFloor;
}

float3 SampleReflectionEnvironment(float3 rayDir, float roughness)
{
    if (g_iblEnabled > 0.5f)
    {
        float mip = saturate(roughness) * g_iblPrefilterMaxMip;
        return g_iblPrefilterTex.SampleLevel(LinearClamp, rayDir, mip).rgb * g_iblIntensity;
    }
    if (g_proceduralSkyEnabled > 0.5f)
    {
        return ComputeSkyColor(rayDir,
                               g_dirLightDir,
                               g_dirLightColor,
                               g_dirLightIntensity);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

float3 ResolveReflectionRayThroughTransparent(float3 rayOrigin,
                                              float3 rayDir,
                                              float maxDistance,
                                              float3 initialThroughput,
                                              out bool resolvedRadiance)
{
    static const uint kMaxTransparentLayers = 4u;
    float3 color = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = initialThroughput;
    float traveled = 0.0f;
    resolvedRadiance = false;

    [loop]
    for (uint layer = 0u; layer < kMaxTransparentLayers; ++layer)
    {
        HitResult hit = TraceClosestHit(rayOrigin, rayDir, g_tMin, max(maxDistance - traveled, 0.0f));
        if (!hit.hit)
        {
            color += throughput * SampleReflectionEnvironment(rayDir, 0.0f);
            resolvedRadiance = true;
            break;
        }

        GpuInstanceInfo inst = g_instances[hit.instanceIndex];
        GpuMaterial mat = g_materials[inst.materialIndex];
        float3 hitPos = rayOrigin + rayDir * hit.t;
        float3 hitNorm = GetWorldNormal(hit);
        if (dot(hitNorm, -rayDir) < 0.0f)
            hitNorm = -hitNorm;

        float3 V = normalize(-rayDir);
        float surfaceOpacity = SWRT_MaterialSurfaceOpacity(mat);
        float3 direct = ShadeDirectPBR(hitPos, hitNorm, V, mat) + EvaluateReflectionHitAmbientFloor(mat);
        color += throughput * direct * surfaceOpacity;
        resolvedRadiance = true;

        if (!SWRT_IsTransparentMaterial(mat) || surfaceOpacity > 0.995f)
            break;

        float3 transmittedDir = refract(rayDir, hitNorm, 1.0f / max(mat.ior, 1.0f));
        if (dot(transmittedDir, transmittedDir) <= 1e-6f)
            transmittedDir = rayDir;
        transmittedDir = normalize(transmittedDir);

        float3 transmittance = SWRT_MaterialTransmittanceTint(mat) * (1.0f - surfaceOpacity);
        throughput *= transmittance;
        if (dot(throughput, float3(0.2126f, 0.7152f, 0.0722f)) < 0.01f)
            break;

        traveled += hit.t;
        rayOrigin = hitPos + transmittedDir * max(g_tMin, 0.01f);
        rayDir = transmittedDir;
    }

    return color;
}

// --------------------------------------------------------------------------

#endif // SASAMI_SWRT_REFLECTION_SHADING_HLSLI
