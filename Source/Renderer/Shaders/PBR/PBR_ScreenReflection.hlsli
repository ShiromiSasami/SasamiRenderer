#ifndef SASAMI_PBR_SCREEN_REFLECTION_HLSLI
#define SASAMI_PBR_SCREEN_REFLECTION_HLSLI

bool TraceScreenSpaceReflection(float3 worldPos,
                                float3 normal,
                                float3 reflectionDir,
                                float roughness,
                                out float2 hitUv,
                                out bool allowSkyIblMiss)
{
    hitUv = 0.0;
    allowSkyIblMiss = false;
    if (u_reflectionParams.x <= 1.5 || u_reflectionParams.z <= 0.5 || u_reflectionParams.w <= 0.5) {
        return false;
    }

    const int stepCount = 48;
    const float maxDistance = lerp(18.0, 7.0, saturate(roughness));
    const float baseThickness = lerp(0.0025, 0.018, saturate(roughness));
    float3 rayOrigin = worldPos + normal * 0.03 + reflectionDir * 0.08;
    bool sawSceneDepth = false;

    [loop]
    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
        float t = (float)stepIndex / (float)stepCount;
        t = t * t;
        float3 samplePos = rayOrigin + reflectionDir * (maxDistance * t);
        float4 sampleClip = mul(float4(samplePos, 1.0), u_cameraPV);
        if (sampleClip.w <= 1e-5) {
            continue;
        }

        float3 sampleNdc = sampleClip.xyz / sampleClip.w;
        if (sampleNdc.z <= 0.0) {
            continue;
        }

        // Screen exits are unknown in a raster SSR pass. Only an in-screen ray
        // reaching the far plane without seeing scene depth is treated as sky.
        if (any(sampleNdc.xy < -1.0) || any(sampleNdc.xy > 1.0)) {
            return false;
        }
        if (sampleNdc.z >= 1.0) {
            allowSkyIblMiss = !sawSceneDepth;
            return false;
        }

        float2 sampleUv = float2(sampleNdc.x * 0.5 + 0.5, -sampleNdc.y * 0.5 + 0.5);
        if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) {
            return false;
        }

        float sceneDepth = SceneDepthTex.SampleLevel(LinearWrap, sampleUv, 0).r;
        if (sceneDepth >= 0.99999 || sceneDepth <= 1e-5) {
            continue;
        }
        sawSceneDepth = true;

        float depthDelta = sampleNdc.z - sceneDepth;
        float thickness = baseThickness + t * 0.01;
        if (depthDelta >= -thickness * 0.25 && depthDelta <= thickness) {
            hitUv = sampleUv;
            return true;
        }
    }

    return false;
}

#endif // SASAMI_PBR_SCREEN_REFLECTION_HLSLI
