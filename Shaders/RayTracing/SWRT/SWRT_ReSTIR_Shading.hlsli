#ifndef SASAMI_SWRT_RESTIR_SHADING_HLSLI
#define SASAMI_SWRT_RESTIR_SHADING_HLSLI

// ReSTIR reflection direct-light BRDF helpers.

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float GGX_D(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}

float GGX_V(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f; float k = (r * r) / 8.0f;
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    return gL * gV;
}

float3 EvalPBR(float3 N, float3 L, float3 V, float3 albedo, float roughness, float metallic,
               float3 lightRadiance)
{
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 H    = normalize(L + V);
    float  NdotL = max(dot(N, L), 0.0f);
    float  NdotV = max(dot(N, V), 0.001f);
    float  NdotH = saturate(dot(N, H));
    float  VdotH = saturate(dot(V, H));
    float3 F   = FresnelSchlick(VdotH, F0);
    float  D   = GGX_D(NdotH, max(roughness, 0.05f));
    float  Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));
    float3 spec = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
    float3 kd   = (1.0f - F) * (1.0f - metallic);
    return (kd * albedo / 3.14159265f + spec) * NdotL * lightRadiance;
}

#endif // SASAMI_SWRT_RESTIR_SHADING_HLSLI
