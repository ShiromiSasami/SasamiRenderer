#ifndef SASAMI_RAYTRACING_SHADING_HLSLI
#define SASAMI_RAYTRACING_SHADING_HLSLI

float Saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

float3 SafeNormalize(float3 value)
{
    float lenSq = dot(value, value);
    if (lenSq <= 1e-8) {
        return float3(0.0, 0.0, 1.0);
    }
    return value * rsqrt(lenSq);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float NdotV, float NdotL, float k)
{
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

float2 WrapUv(float2 uv)
{
    return frac(uv);
}

float4 SampleBaseColor(MaterialData material, float2 uv, float4 fallbackColor)
{
    const float4 baseColor = fallbackColor * material.baseColor;
    if (material.albedoDescriptorIndex < 0) {
        return baseColor;
    }

    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoDescriptorIndex)];
    return texture.SampleLevel(LinearWrapSampler, WrapUv(uv), 0) * baseColor;
}

float SampleAmbientOcclusion(MaterialData material, float2 uv)
{
    float ao = 1.0;
    if (material.occlusionDescriptorIndex < 0) {
        return ao;
    }

    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.occlusionDescriptorIndex)];
    ao = texture.SampleLevel(LinearWrapSampler, WrapUv(uv), 0).r;
    return lerp(1.0, ao, saturate(material.emissiveOcclusionStrength.w));
}

float3 SkyColor(float3 rayDirection)
{
    float t = Saturate(rayDirection.y * 0.5 + 0.5);
    return lerp(float3(0.10, 0.12, 0.18), float3(0.50, 0.62, 0.90), t);
}

float3 ApplyDirectionalLightMarker(float3 color, float3 sampleDir)
{
    if (gFrame.directionalLightMarkerParams.x <= 0.5 || gFrame.directionalLightColorIntensity.a <= 0.0) {
        return color;
    }

    const float3 lightDir = SafeNormalize(gFrame.directionalLightDirection.xyz);
    const float cosTheta = Saturate(dot(SafeNormalize(sampleDir), lightDir));
    const float discMask = smoothstep(cos(gFrame.directionalLightMarkerParams.y), 1.0, cosTheta);
    const float haloMask = smoothstep(cos(gFrame.directionalLightMarkerParams.z), 1.0, cosTheta);
    const float3 markerColor =
        lerp(saturate(gFrame.directionalLightColorIntensity.rgb), float3(1.0, 1.0, 1.0), 0.35);

    return color + markerColor * ((haloMask * 0.5) + (discMask * 6.0)) * gFrame.directionalLightMarkerParams.w;
}

float3 EvaluateBrdf(float3 normal,
                    float3 view,
                    float3 lightDir,
                    float3 lightColor,
                    float3 albedo,
                    float roughness,
                    float metallic)
{
    float a = roughness * roughness;
    float k = ((roughness + 1.0) * (roughness + 1.0)) / 8.0;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 halfVector = SafeNormalize(view + lightDir);
    float NdotV = Saturate(dot(normal, view));
    float NdotL = Saturate(dot(normal, lightDir));
    if (NdotL <= 0.0) {
        return 0.0.xxx;
    }
    if (gFrame.qualityTier != 0u) {
        return albedo / 3.14159265 * NdotL * lightColor;
    }

    float NdotH = Saturate(dot(normal, halfVector));
    float VdotH = Saturate(dot(view, halfVector));
    float D = DistributionGGX(NdotH, a);
    float G = GeometrySmith(NdotV, NdotL, k);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo / 3.14159265;
    return (diffuse + specular) * NdotL * lightColor;
}

#endif // SASAMI_RAYTRACING_SHADING_HLSLI
