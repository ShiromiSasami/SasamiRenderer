#ifndef SASAMI_SWRT_RESTIR_LIGHT_EVAL_HLSLI
#define SASAMI_SWRT_RESTIR_LIGHT_EVAL_HLSLI

// Shared ReSTIR target-function evaluation for a light index `i` (point lights
// first, then spot lights, matching the reservoir sampling order).
//
// Must be included after:
//   - SWRT_LightTypes.hlsli (GpuPointLightRT / GpuSpotLightRT)
//   - SWRT_Reservoir.hlsli (PhatPoint)
//   - g_pointLights (t12), g_spotLights (t13), g_pointLightCount, g_spotLightCount

float EvalPhat(uint i, float3 pos, float3 N)
{
    if (i == 0xFFFFFFFFu) return 0.0f;
    uint totalLights = g_pointLightCount + g_spotLightCount;
    if (i >= totalLights) return 0.0f;
    if (i < g_pointLightCount)
    {
        GpuPointLightRT pl = g_pointLights[i];
        return PhatPoint(pos, N, pl.pos, pl.colorIntensity, pl.range);
    }
    uint si = i - g_pointLightCount;
    GpuSpotLightRT sl = g_spotLights[si];
    float3 toLight = sl.pos - pos;
    float  dist    = length(toLight);
    if (dist >= sl.range) return 0.0f;
    float3 L = toLight / dist;
    float  cosA = dot(-L, normalize(sl.dir));
    if (cosA < sl.cosOuter) return 0.0f;
    float spotA = smoothstep(sl.cosOuter, sl.cosInner, cosA);
    return PhatPoint(pos, N, sl.pos, sl.colorIntensity * spotA, sl.range);
}

#endif // SASAMI_SWRT_RESTIR_LIGHT_EVAL_HLSLI
