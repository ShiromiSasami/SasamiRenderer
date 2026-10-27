#ifndef SASAMI_PBR_SHADOW_HLSLI
#define SASAMI_PBR_SHADOW_HLSLI

#include "Shared/Common/MicroShadowing.hlsli"

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
    // u_shadowCascadeParams.z (m_shadowNormalBias) is a world-space offset, and it was applied
    // identically to every cascade. Each further cascade covers more world per shadow texel, so
    // the same absolute offset lifts the sample proportionally less clear of the surface and the
    // far cascades acne while cascade 0 is correctly tuned. Scale by how much coarser this
    // cascade's texel is than cascade 0's, which leaves cascade 0 exactly as it was.
    const float baseTexelWorld = max(u_shadowCascadeTexelSize[0].x, 1e-6);
    const float texelWorld     = max(u_shadowCascadeTexelSize[cascadeIndex].x, 1e-6);
    float normalBias = max(u_shadowCascadeParams.z, 0.0) * (texelWorld / baseTexelWorld);
    float3 biasedWorldPos = worldPos + normal * normalBias;
    float4 lightPos = mul(float4(biasedWorldPos, 1.0), u_lightVP[cascadeIndex]);
    float3 sc = lightPos.xyz / max(lightPos.w, 1e-6);
    float2 suv = float2(sc.x * 0.5 + 0.5, -sc.y * 0.5 + 0.5);  // NDC->UV (Y flipped)
    float  sdepth = sc.z;
    if (any(suv < 0.0) || any(suv > 1.0)) return 1.0;

    const float bias  = u_shadowCascadeTexelSize[cascadeIndex].z +
                        u_shadowCascadeTexelSize[cascadeIndex].w * (1.0 - saturate(NdotL));
    float2 texel  = max(u_shadowParams.xy, float2(1e-5, 1e-5));
    // PCF disk radius in texels (default 2.0) at cascade 0. u_shadowCascadeTexelSize.x is the
    // world-space size of one texel, so scaling by texel0 / texelN keeps the world footprint
    // of the kernel constant across cascades. Floored at the old 0.5 texel minimum.
    float  radius = max(max(u_shadowParams.z, 0.5) * (baseTexelWorld / texelWorld), 0.5);

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
        // Reverse-Z: nearer means a LARGER depth, so the receiver is lit when it is at or
        // in front of the caster, and the bias has to push it toward the light by adding
        // rather than subtracting. ShadowCmp (GREATER_EQUAL, clamp) compares each texel
        // before filtering, so the bilinear footprint averages test results, not depths.
        acc += ShadowMapTex.SampleCmpLevelZero(ShadowCmp, float3(uv, cascadeIndex), sdepth + bias);
    }
    return acc / 16.0;
}

float SampleShadowVSMCascade(float3 worldPos, float3 normal, int cascadeIdx)
{
    // Scaled per cascade for the same reason as SampleShadowCascade above: a constant world
    // offset that clears cascade 0's texel barely clears the far cascade's much larger one.
    const float baseTexelWorld = max(u_shadowCascadeTexelSize[0].x, 1e-6);
    const float texelWorld     = max(u_shadowCascadeTexelSize[cascadeIdx].x, 1e-6);
    float normalBias = max(u_shadowCascadeParams.z, 0.0) * (texelWorld / baseTexelWorld);
    float3 biasPos = worldPos + normal * normalBias;
    float4 lp = mul(float4(biasPos, 1.0), u_lightVP[cascadeIdx]);
    float3 sc = lp.xyz / max(lp.w, 1e-6);
    float2 uv = float2(sc.x * 0.5 + 0.5, -sc.y * 0.5 + 0.5);
    float  depth = sc.z;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    float2 moments = ShadowVSMTex.SampleLevel(LinearWrap, float3(uv, cascadeIdx), 0);
    // Reverse-Z: lit when the receiver is at or in front of the stored mean depth.
    float p = (depth >= moments.x) ? 1.0 : 0.0;
    float variance = max(moments.y - moments.x * moments.x, u_vsmParams.z);
    float d = depth - moments.x;
    float pMax = variance / (variance + d * d);
    float lbr = u_vsmParams.y;
    pMax = saturate((pMax - lbr) / max(1.0 - lbr, 1e-4));
    return max(p, pMax);
}

// The cascade splits are uploaded as reverse-Z NDC depth, which is hyperbolic in view depth, so
// a blend fraction taken in NDC space gives bands whose world width varies wildly with range.
// Unprojecting (0, 0, z, 1) through the inverse camera PV yields w = 1 / viewDepth, so the
// splits can be linearised here without any extra constant-buffer data.
float LinearViewDepthFromNdcZ(float ndcZ)
{
    float invW = mul(float4(0.0, 0.0, ndcZ, 1.0), u_invCameraPV).w;
    return 1.0 / max(invW, 1e-8);
}

// Blend weight of cascade (cascadeIndex + 1) over cascadeIndex, with the blend band measured
// as a fraction of the cascade's depth range in linear view depth. 0 outside the band.
float ComputeCascadeBlendFactor(float3 worldPos, int cascadeIndex)
{
    const float viewDepth = mul(float4(worldPos, 1.0), u_cameraPV).w;
    const float cascadeNear = LinearViewDepthFromNdcZ((cascadeIndex == 0) ? 1.0 : u_shadowCascadeSplits[cascadeIndex - 1]);
    const float cascadeFar = LinearViewDepthFromNdcZ(u_shadowCascadeSplits[cascadeIndex]);
    const float blendWidth = max((cascadeFar - cascadeNear) * u_shadowCascadeParams.y, 1e-4);
    const float blendStart = cascadeFar - blendWidth;
    return saturate((viewDepth - blendStart) / blendWidth);
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
        // Reverse-Z makes cameraDepth decrease with distance, so the split list is
        // descending and the pixel belongs to the first cascade it is still in front of.
        if (cameraDepth >= u_shadowCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }
    if (cascadeIndex < 0) return 1.0;

    float visibility = SampleShadowVSMCascade(worldPos, normal, cascadeIndex);
    if (cascadeIndex >= cascadeCount - 1) return visibility;

    const float t = ComputeCascadeBlendFactor(worldPos, cascadeIndex);
    if (t <= 0.0) return visibility;

    const float nextVis = SampleShadowVSMCascade(worldPos, normal, cascadeIndex + 1);
    return lerp(visibility, nextVis, t);
}

// Cascade choice, split out of ShadowVisibility so the ShadowCascadeIndex debug view can
// report exactly which cascade a pixel samples without restating the selection rule. Returns
// -1 when the pixel falls past the last cascade, which ShadowVisibility treats as fully lit.
// cameraDepth is handed back because the cascade-blend below needs the same value.
int SelectShadowCascade(float3 worldPos, out float cameraDepth)
{
    float4 cameraClip = mul(float4(worldPos, 1.0), u_cameraPV);
    cameraDepth = cameraClip.z / max(cameraClip.w, 1e-6);
    const int cascadeCount = max((int)(u_shadowCascadeParams.w + 0.5), 1);

    [unroll]
    for (int i = 0; i < DIRECTIONAL_SHADOW_CASCADE_COUNT; ++i) {
        if (i >= cascadeCount) {
            break;
        }
        // See the VSM path above: reverse-Z makes the split list descending.
        if (cameraDepth >= u_shadowCascadeSplits[i]) {
            return i;
        }
    }
    return -1;
}

int SelectShadowCascade(float3 worldPos)
{
    float unusedDepth;
    return SelectShadowCascade(worldPos, unusedDepth);
}

// Flat per-cascade colours for the ShadowCascadeIndex debug view. Kept beside the selection
// rule so the two cannot drift: red/green/blue/yellow for cascades 0..3, magenta for a pixel
// past the last cascade (which ShadowVisibility renders fully lit).
float3 ShadowCascadeDebugColor(float3 worldPos)
{
    const int cascadeIndex = SelectShadowCascade(worldPos);
    if (cascadeIndex == 0) { return float3(1.0, 0.0, 0.0); }
    if (cascadeIndex == 1) { return float3(0.0, 1.0, 0.0); }
    if (cascadeIndex == 2) { return float3(0.0, 0.0, 1.0); }
    if (cascadeIndex == 3) { return float3(1.0, 1.0, 0.0); }
    return float3(1.0, 0.0, 1.0);
}

float ShadowVisibility(float3 worldPos, float3 normal, float3 lightDir, float NdotL)
{
    // u_dirColor.w: directional shadow source mode (see LightCBData.h / LightSystem::
    // SetDirectionalShadowSourceState). 2 = SWRT is enabled but its texture is not trustworthy
    // this frame (never rendered / stale fallback bound at ShadowMapTex) - stay unshadowed
    // rather than sample it. 0 (raster) and 1 (SWRT, trustworthy) both fall through unchanged:
    // when SWRT is active, LightSystem::BuildStableDirectionalShadowPassContext already uploads
    // cascadeCount == 1, so the existing cascade-selection loop below naturally samples cascade
    // index 0 (the single SWRT-rendered light-space slice) without any further branching.
    if (u_dirColor.w > 1.5) {
        return 1.0;
    }

    if (u_vsmParams.x > 1.5) {
        return ShadowVisibilityVSM(worldPos, normal, NdotL);
    }

    float cameraDepth;
    const int cascadeIndex = SelectShadowCascade(worldPos, cameraDepth);
    const int cascadeCount = max((int)(u_shadowCascadeParams.w + 0.5), 1);
    if (cascadeIndex < 0) {
        return 1.0;
    }

    float visibility = SampleShadowCascade(worldPos, normal, lightDir, cascadeIndex, NdotL);
    if (cascadeIndex >= cascadeCount - 1) {
        return visibility;
    }

    const float t = ComputeCascadeBlendFactor(worldPos, cascadeIndex);
    if (t <= 0.0) {
        return visibility;
    }

    const float nextVisibility = SampleShadowCascade(worldPos, normal, lightDir, cascadeIndex + 1, NdotL);
    return lerp(visibility, nextVisibility, t);
}

float SamplePointShadow(float3 worldPos, float3 normal, float3 lightPos, int shadowIndex)
{
    if (u_pointShadowParams.z <= 0.5 || shadowIndex < 0) {
        return 1.0;
    }
    int clampedIndex = min(max(shadowIndex, 0), MAX_POINT_SHADOWS - 1);

    float dist = length(worldPos - lightPos);
    float3 biasedWorldPos = worldPos + normal * (u_pointShadowParams.y * dist);

    float3 d = biasedWorldPos - lightPos;
    float3 ad = abs(d);
    uint faceIndex;
    if (ad.x >= ad.y && ad.x >= ad.z) {
        faceIndex = (d.x >= 0.0) ? 0 : 1;
    } else if (ad.y >= ad.x && ad.y >= ad.z) {
        faceIndex = (d.y >= 0.0) ? 2 : 3;
    } else {
        faceIndex = (d.z >= 0.0) ? 4 : 5;
    }

    float4 sc = mul(float4(biasedWorldPos, 1.0), u_pointLightVP[clampedIndex * 6 + faceIndex]);
    sc.xyz /= sc.w;
    float2 shadowUV = sc.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    float shadowDepth = sc.z + u_pointShadowParams.x; // depth bias (added: reverse-Z pushes toward the light)

    if (!(all(shadowUV >= 0.0) && all(shadowUV <= 1.0) && sc.w > 0.0)) {
        return 1.0;
    }

    float sliceIndex = float(clampedIndex * 6 + faceIndex);
    float texelSize = 1.0 / u_pointShadowParams.w;
    float shadowSum = 0.0;
    [unroll] for (int sy = -1; sy <= 1; ++sy)
    [unroll] for (int sx = -1; sx <= 1; ++sx)
    {
        // Compare-then-average: SampleCmpLevelZero runs the depth test per texel and
        // returns the filtered *occlusion*. Sampling the depth map with a plain linear
        // sampler and thresholding afterwards would interpolate depth values instead,
        // which is not PCF and cannot produce a correct penumbra.
        // The ShadowCmp sampler clamps, so an edge tap stays on this cube face rather
        // than folding around to the opposite side (the range test above only guards
        // the centre tap, not these eight neighbours).
        float2 tapUV = shadowUV + float2(sx, sy) * texelSize;
        shadowSum += PointShadowMapTex.SampleCmpLevelZero(ShadowCmp, float3(tapUV, sliceIndex), shadowDepth).r;
    }
    return shadowSum / 9.0;
}

// Spot light shadow, 3x3 PCF. Shares one definition with the deferred path: this block
// used to be copy-pasted into CookTorranceGGX_PS.hlsl and DeferredLighting_PS.hlsl, where
// the two copies could silently drift apart.
// SpotShadowMapTex / u_spotLightVP / u_spotShadowParams are declared by the including shader.
float SampleSpotShadow(float3 worldPos, float3 normal, float distToLight, int shadowIndex)
{
    if (shadowIndex < 0 || u_spotShadowParams.z <= 0.5) {
        return 1.0;
    }
    int clampedIndex = min(max(shadowIndex, 0), MAX_SPOT_SHADOWS - 1);

    float3 biasedWorldPos = worldPos + normal * (u_spotShadowParams.y * distToLight);
    float4 sc = mul(float4(biasedWorldPos, 1.0), u_spotLightVP[clampedIndex]);
    sc.xyz /= sc.w;
    float2 shadowUV = sc.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    float shadowDepth = sc.z + u_spotShadowParams.x; // depth bias (added: reverse-Z pushes toward the light)

    if (!(all(shadowUV >= 0.0) && all(shadowUV <= 1.0) && sc.w > 0.0)) {
        return 1.0;
    }

    float texelSize = 1.0 / u_spotShadowParams.w;
    float shadowSum = 0.0;
    [unroll] for (int sy = -1; sy <= 1; ++sy)
    [unroll] for (int sx = -1; sx <= 1; ++sx)
    {
        // Compare-then-average via the clamped comparison sampler; see SamplePointShadow
        // above for why thresholding a linearly-filtered depth is not PCF.
        float2 tapUV = shadowUV + float2(sx, sy) * texelSize;
        shadowSum += SpotShadowMapTex.SampleCmpLevelZero(ShadowCmp, float3(tapUV, clampedIndex), shadowDepth).r;
    }
    return shadowSum / 9.0;
}

// Screen-space contact shadow, marched in world space and compared in LINEAR depth.
//
// The comparison used to be done directly on reverse-Z NDC depth
// (`sceneDepth - thickness > sampleNdc.z`) with `thickness` expressed in NDC units.
// NDC depth goes as ~near/distance, so a single NDC value means a wildly different
// world thickness depending on range: with near = 0.0122 a 0.0025 NDC window is
// ~20 cm of world separation at 1 m but ~20 m at 10 m. That is what made the effect
// vanish up close (it demanded an impossibly large separation), snap on at mid range,
// and swing whenever the camera moved. UE's ShadowRayCast and Filament's
// screenSpaceContactShadow both compare in linear depth; this does the same, in metres.
float ComputeContactShadowAlongRay(float3 worldPos, float3 normal, float3 rayDir, float maxDistanceIn)
{
    // u_reflectionParams.zw is screen resolution in pixels, not a flag; it was never a
    // meaningful gate here (always > 0.5 for any real viewport). u_contactShadowParams.x
    // is the real enable/disable toggle.
    if (u_contactShadowParams.x < 0.5) {
        return 1.0;
    }

    const int stepCount = max((int)u_contactShadowParams.w, 1);
    const float maxDistance = max(maxDistanceIn, 1e-3);
    // World-space (metres) occluder thickness. A sample only counts as shadowed while it
    // sits between the visible surface and `thickness` behind it; bounding the window is
    // what stops a near silhouette from shadowing the whole background, and is Filament's
    // `abs(tolerance - dz) < tolerance` test in world units.
    const float thickness = max(u_contactShadowParams.z, 1e-3);
    const float stepLength = maxDistance / stepCount;

    // Offset the ray origin slightly to reduce immediate self hits.
    float3 rayOrigin = worldPos + normal * stepLength * 0.25;

    const float originDistance = length(rayOrigin - u_cameraPos.xyz);
    const float endDistance = length(rayOrigin + rayDir * maxDistance - u_cameraPos.xyz);

    // Filament derives its tolerance from the ray's own view-depth span divided by the
    // step count, which guarantees the acceptance window is never narrower than the depth
    // the ray crosses in one step (otherwise a true contact can fall between two samples).
    // Keep that as a floor on the window; the user thickness is the other floor, and takes
    // over when the ray runs perpendicular to the view direction and the span collapses.
    const float perStepDepthSpan = abs(endDistance - originDistance) / stepCount;
    const float depthWindow = max(thickness, 2.0 * perStepDepthSpan);

    // Depth-buffer precision and the inverse-PV round-trip both degrade with distance, so
    // the self-intersection guard has to be relative rather than a fixed epsilon.
    const float selfBias = max(originDistance * 2.0e-4, 5.0e-4);

    // Per-pixel start jitter in [0, 1) of one step, Interleaved Gradient Noise (Jimenez 2014,
    // "Next Generation Post Processing in Call of Duty: Advanced Warfare"). The pixel position
    // is recovered from the receiver's own screen projection (u_reflectionParams.zw = screen
    // size). The constant buffer has no frame index, so this is spatial-only (no temporal term).
    float2 pixelPos = float2(0.0, 0.0);
    {
        float4 receiverClip = mul(float4(worldPos, 1.0), u_cameraPV);
        if (receiverClip.w > 1e-5) {
            float2 receiverUv = float2(receiverClip.x / receiverClip.w * 0.5 + 0.5,
                                       -receiverClip.y / receiverClip.w * 0.5 + 0.5);
            pixelPos = floor(receiverUv * u_reflectionParams.zw);
        }
    }
    const float startJitter = frac(52.9829189 * frac(dot(pixelPos, float2(0.06711056, 0.00583715))));

    [loop]
    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex) {
        float3 samplePos = rayOrigin + rayDir * (stepLength * (stepIndex - 1.0 + startJitter));
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
        // Reverse-Z: the near plane is 1 and the far plane is 0. Both ends are degenerate --
        // a depth sitting exactly on the near plane carries no usable occlusion, and the far
        // plane means nothing was ever written there (sky), so there is nothing to occlude with.
        if (sceneDepth >= 1.0 - 1e-5 || sceneDepth <= 1e-6) {
            continue;
        }

        // The visible surface at `sampleUv` and `samplePos` are collinear with the camera
        // (sampleUv *is* samplePos projected), so the difference of their camera distances is
        // their exact world-space separation along that view ray. This needs no near/far
        // constants and is linear by construction.
        float4 sceneClip = float4(sampleNdc.xy, sceneDepth, 1.0);
        float4 sceneWorldH = mul(sceneClip, u_invCameraPV);
        float3 scenePos = sceneWorldH.xyz / max(sceneWorldH.w, 1e-6);

        float sceneDistance = length(scenePos - u_cameraPos.xyz);
        float sampleDistance = length(samplePos - u_cameraPos.xyz);
        // Positive means the sample lies behind the visible surface, i.e. it is occluded.
        float delta = sampleDistance - sceneDistance;

        if (delta > selfBias && delta < selfBias + depthWindow) {
            return 0.0;
        }
    }

    return 1.0;
}

float ComputeDirectionalContactShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    return ComputeContactShadowAlongRay(worldPos, normal, lightDir, u_contactShadowParams.y);
}

// Screen-space contact shadow for point/spot lights (UE contact shadows /
// Filament punctual screenSpaceContactShadow): march toward the light position,
// never past it.
float ComputePunctualContactShadow(float3 worldPos, float3 normal, float3 lightDir, float distToLight)
{
    return ComputeContactShadowAlongRay(worldPos, normal, lightDir,
                                        min(u_contactShadowParams.y, distToLight));
}

#endif // SASAMI_PBR_SHADOW_HLSLI
