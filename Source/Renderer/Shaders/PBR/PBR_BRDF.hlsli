#ifndef SASAMI_PBR_BRDF_HLSLI
#define SASAMI_PBR_BRDF_HLSLI

// --- PBR helpers (Cook-Torrance microfacet model) ---
// D_GGX(N,H,alpha) = alpha^2 / (PI * ((N.H)^2 * (alpha^2 - 1) + 1)^2)
float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-4);
}

// Schlick-GGX geometry approximation:
// G1(v) = (N.V) / ((N.V) * (1 - k) + k)
float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

// Smith masking-shadowing for both view/light:
// G_Smith = G1(N.V) * G1(N.L)
float GeometrySmith(float NdotV, float NdotL, float k)
{
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// Schlick Fresnel approximation:
// F(cosTheta) = F0 + (1 - F0) * (1 - cosTheta)^5
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// Specular occlusion approximation for image-based specular term.
// One practical form used in real-time renderers:
// SO = saturate((N.V + AO)^(2^(-16*roughness - 1)) - 1 + AO)
float SpecularOcclusion(float ao, float NdotV, float roughness)
{
    const float exponent = exp2(-16.0 * roughness - 1.0);
    return saturate(pow(saturate(NdotV + ao), exponent) - 1.0 + ao);
}

float ReflectionMaterialMask(float reflectionStrength)
{
    return saturate(reflectionStrength);
}

#endif // SASAMI_PBR_BRDF_HLSLI
