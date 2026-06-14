#ifndef SASAMI_SWRT_MATERIAL_HLSLI
#define SASAMI_SWRT_MATERIAL_HLSLI

// Shared material interpretation helpers.

bool SWRT_UseSpecularGlossiness(GpuMaterial mat)
{
    return mat.workflow > 0.5f;
}

float3 SWRT_MaterialF0(GpuMaterial mat)
{
    return SWRT_UseSpecularGlossiness(mat)
        ? saturate(mat.specularColor)
        : lerp(float3(0.04f, 0.04f, 0.04f), mat.baseColor.rgb, saturate(mat.metallic));
}

float3 SWRT_MaterialDiffuseReflectance(GpuMaterial mat)
{
    const float transmissionScale = 1.0f - saturate(mat.transmission);
    if (SWRT_UseSpecularGlossiness(mat))
    {
        float specMax = max(max(mat.specularColor.r, mat.specularColor.g), mat.specularColor.b);
        return mat.baseColor.rgb * saturate(1.0f - specMax) * transmissionScale;
    }
    return mat.baseColor.rgb * (1.0f - saturate(mat.metallic)) * transmissionScale;
}

float SWRT_MaterialAlpha(GpuMaterial mat)
{
    return saturate(mat.baseColor.a);
}

float SWRT_MaterialEffectiveTransmission(GpuMaterial mat)
{
    return saturate(mat.transmission) * (1.0f - saturate(mat.metallic));
}

bool SWRT_IsTransparentMaterial(GpuMaterial mat)
{
    return SWRT_MaterialAlpha(mat) < 0.999f || SWRT_MaterialEffectiveTransmission(mat) > 0.001f;
}

float SWRT_MaterialSurfaceOpacity(GpuMaterial mat)
{
    const float alphaOpacity = SWRT_MaterialAlpha(mat);
    const float transmissionOpacity = 1.0f - SWRT_MaterialEffectiveTransmission(mat);
    return saturate(min(alphaOpacity, transmissionOpacity));
}

float3 SWRT_MaterialTransmittanceTint(GpuMaterial mat)
{
    const float transmission = SWRT_MaterialEffectiveTransmission(mat);
    return lerp(float3(1.0f, 1.0f, 1.0f), saturate(mat.baseColor.rgb), transmission * 0.35f);
}

// --------------------------------------------------------------------------

#endif // SASAMI_SWRT_MATERIAL_HLSLI
