#ifndef SASAMI_PBR_SHADOW_HLSLI
#define SASAMI_PBR_SHADOW_HLSLI

// 16-tap Poisson disk kernel (van der Corput layout, unit disk).
// Gives soft, alias-free shadow edges when combined with per-pixel rotation.
static const float2 kPoisson16[16] =
{
    float2(-0.94201624f, -0.39906216f), float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f), float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f), float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f), float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f), float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f), float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f), float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f), float2( 0.14383161f, -0.14100790f),
};

float SampleShadowCascade(float3 worldPos, float3 normal, float3 lightDir, int cascadeIndex, float NdotL)
{
    float normalBias = max(u_shadowCascadeParams.z, 0.0);
    float3 biasedWorldPos = worldPos + normal * normalBias;
    float4 lightPos = mul(float4(biasedWorldPos, 1.0), u_lightVP[cascadeIndex]);
    float3 sc = lightPos.xyz / max(lightPos.w, 1e-6);
    float2 suv = float2(sc.x * 0.5 + 0.5, -sc.y * 0.5 + 0.5);  // NDC->UV (Y flipped)
    float  sdepth = sc.z;
    if (any(suv < 0.0) || any(suv > 1.0)) return 1.0;

    const float bias  = u_shadowCascadeTexelSize[cascadeIndex].z +
                        u_shadowCascadeTexelSize[cascadeIndex].w * (1.0 - saturate(NdotL));
    float2 texel  = max(u_shadowParams.xy, float2(1e-5, 1e-5));
    float  radius = max(u_shadowParams.z,  0.5);  // PCF disk radius in texels (default 2.0)

    // Anchor the PCF rotation to the shadow-map texel instead of continuous UV.
    // Continuous-UV hashing changes whenever the camera-fitted light projection
    // moves, causing visible shadow shimmer during camera movement.
    float2 shadowTexel = floor(suv / texel + 0.5f);
    float2 cascadeSalt = float2(37.0f, 17.0f) * float(cascadeIndex + 1);
    float  angle = frac(sin(dot(shadowTexel + cascadeSalt, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.28318f;
    float  sa = sin(angle), ca = cos(angle);

    float acc = 0.0;
    [unroll]
    for (int k = 0; k < 16; ++k)
    {
        float2 d  = kPoisson16[k];
        float2 rd = float2(ca * d.x - sa * d.y, sa * d.x + ca * d.y) * radius;
        float2 uv = saturate(suv + rd * texel);
        float sd  = ShadowMapTex.SampleLevel(LinearWrap, float3(uv, cascadeIndex), 0).r;
        acc += (sdepth - bias <= sd) ? 1.0 : 0.0;
    }
    return acc / 16.0;
}

float SampleShadowVSMCascade(float3 worldPos, float3 normal, int cascadeIdx)
{
    float normalBias = max(u_shadowCascadeParams.z, 0.0);
    float3 biasPos = worldPos + normal * normalBias;
    float4 lp = mul(float4(biasPos, 1.0), u_lightVP[cascadeIdx]);
    float3 sc = lp.xyz / max(lp.w, 1e-6);
    float2 uv = float2(sc.x * 0.5 + 0.5, -sc.y * 0.5 + 0.5);
    float  depth = sc.z;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    float2 moments = ShadowVSMTex.SampleLevel(LinearWrap, float3(uv, cascadeIdx), 0);
    float p = (depth <= moments.x) ? 1.0 : 0.0;
    float variance = max(moments.y - moments.x * moments.x, u_vsmParams.z);
    float d = depth - moments.x;
    float pMax = variance / (variance + d * d);
    float lbr = u_vsmParams.y;
    pMax = saturate((pMax - lbr) / max(1.0 - lbr, 1e-4));
    return max(p, pMax);
}

float ShadowVisibilityVSM(float3 worldPos, float3 normal, float NdotL)
{
    float4 cameraClip = mul(float4(worldPos, 1.0), u_cameraPV);
    float cameraDepth = cameraClip.z / max(cameraClip.w, 1e-6);
    const int cascadeCount = max((int)(u_shadowCascadeParams.w + 0.5), 1);

    int cascadeIndex = -1;
    [unroll]
    for (int i = 0; i < DIRECTIONAL_SHADOW_CASCADE_COUNT; ++i) {
        if (i >= cascadeCount) break;
        if (cameraDepth <= u_shadowCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }
    if (cascadeIndex < 0) return 1.0;

    float visibility = SampleShadowVSMCascade(worldPos, normal, cascadeIndex);
    if (cascadeIndex >= cascadeCount - 1) return visibility;

    const float cascadeNear = (cascadeIndex == 0) ? 0.0 : u_shadowCascadeSplits[cascadeIndex - 1];
    const float cascadeFar  = u_shadowCascadeSplits[cascadeIndex];
    const float blendWidth  = max((cascadeFar - cascadeNear) * u_shadowCascadeParams.y, 1e-4);
    const float blendStart  = cascadeFar - blendWidth;
    if (cameraDepth <= blendStart) return visibility;

    const float nextVis = SampleShadowVSMCascade(worldPos, normal, cascadeIndex + 1);
    const float t = saturate((cameraDepth - blendStart) / blendWidth);
    return lerp(visibility, nextVis, t);
}

float ShadowVisibility(float3 worldPos, float3 normal, float3 lightDir, float NdotL)
{
    if (u_vsmParams.x > 1.5) {
        return ShadowVisibilityVSM(worldPos, normal, NdotL);
    }

    float4 cameraClip = mul(float4(worldPos, 1.0), u_cameraPV);
    float cameraDepth = cameraClip.z / max(cameraClip.w, 1e-6);
    const int cascadeCount = max((int)(u_shadowCascadeParams.w + 0.5), 1);

    int cascadeIndex = -1;
    [unroll]
    for (int i = 0; i < DIRECTIONAL_SHADOW_CASCADE_COUNT; ++i) {
        if (i >= cascadeCount) {
            break;
        }
        if (cameraDepth <= u_shadowCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }
    if (cascadeIndex < 0) {
        return 1.0;
    }

    float visibility = SampleShadowCascade(worldPos, normal, lightDir, cascadeIndex, NdotL);
    if (cascadeIndex >= cascadeCount - 1) {
        return visibility;
    }

    const float cascadeNear = (cascadeIndex == 0) ? 0.0 : u_shadowCascadeSplits[cascadeIndex - 1];
    const float cascadeFar = u_shadowCascadeSplits[cascadeIndex];
    const float blendWidth = max((cascadeFar - cascadeNear) * u_shadowCascadeParams.y, 1e-4);
    const float blendStart = cascadeFar - blendWidth;
    if (cameraDepth <= blendStart) {
        return visibility;
    }

    const float nextVisibility = SampleShadowCascade(worldPos, normal, lightDir, cascadeIndex + 1, NdotL);
    const float t = saturate((cameraDepth - blendStart) / blendWidth);
    return lerp(visibility, nextVisibility, t);
}

float ComputeDirectionalContactShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    if (u_contactShadowParams.x < 0.5 || u_reflectionParams.z <= 0.5 || u_reflectionParams.w <= 0.5) {
        return 1.0;
    }

    const int stepCount = max((int)u_contactShadowParams.w, 1);
    const float maxDistance = max(u_contactShadowParams.y, 1e-3);
    const float thickness = max(u_contactShadowParams.z, 1e-4);
    const float stepLength = maxDistance / stepCount;

    // Offset the ray origin slightly to reduce immediate self hits.
    float3 rayOrigin = worldPos + normal * stepLength * 0.25;

    [loop]
    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
        float3 samplePos = rayOrigin + lightDir * (stepLength * stepIndex);
        float4 sampleClip = mul(float4(samplePos, 1.0), u_cameraPV);
        if (sampleClip.w <= 1e-5) {
            continue;
        }

        float3 sampleNdc = sampleClip.xyz / sampleClip.w;
        float2 sampleUv = float2(sampleNdc.x * 0.5 + 0.5, -sampleNdc.y * 0.5 + 0.5);
        if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) {
            continue;
        }

        float sceneDepth = SceneDepthTex.SampleLevel(LinearWrap, sampleUv, 0).r;
        if (sceneDepth <= 1e-5) {
            continue;
        }

        if (sceneDepth + thickness < sampleNdc.z) {
            return 0.0;
        }
    }

    return 1.0;
}

#endif // SASAMI_PBR_SHADOW_HLSLI
