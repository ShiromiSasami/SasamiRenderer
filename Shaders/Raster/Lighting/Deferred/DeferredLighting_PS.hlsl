#include "Shared/Common/LightCB.hlsli"
#include "Raster/Lighting/PBR/PBR_LightTypes.hlsli"

Texture2D<float4> GBufferAlbedoTex   : register(t0);
Texture2D<float4> GBufferNormalTex   : register(t1);
Texture2D<float4> GBufferMaterialTex : register(t2);
Texture2D<float4> GBufferEmissiveTex : register(t3);
Texture2DArray ShadowMapTex          : register(t4);
Texture2D SceneDepthTex              : register(t5);
StructuredBuffer<PointLight> u_pointLights : register(t6);
StructuredBuffer<SpotLight> u_spotLights   : register(t7);
TextureCube IrradianceTex            : register(t8);
Texture2D RuntimeAOTex               : register(t9);
// Root parameter 9 of the deferred root signature already binds the reflection SRV here
// (RenderPipelineStateCache.cpp, deferredBaseRegisters). The shader simply never declared it,
// so this pass could not tell how much indirect specular the composite pass was about to add.
Texture2D ReflectionTex              : register(t11);
Texture2DArray SpotShadowMapTex      : register(t12);
Texture2DArray PointShadowMapTex     : register(t16);
Texture2DArray<float2> ShadowVSMTex  : register(t13);
Texture2D<float> TransparentBackfaceDistanceTex : register(t14);
Texture2D<float4> GBufferSpecularWorkflowTex : register(t15);
TextureCube IblPrefilterTex          : register(t17);
Texture2D IblBrdfLutTex              : register(t18);
SamplerState LinearWrap : register(s0);

// s1: depth comparison sampler for shadow PCF (clamped, LESS_EQUAL). See MakeShadowComparisonSampler.
SamplerComparisonState ShadowCmp : register(s1);
#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"
#include "Raster/Lighting/PBR/PBR_IBL.hlsli"
#include "Raster/Lighting/PBR/PBR_Shadow.hlsli"
#include "Raster/Lighting/PBR/PBR_DirectLighting.hlsli"
#include "RayTracing/GI/GI_Common.hlsli"

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#define u_cameraInvPV u_invCameraPV
#include "Shared/Common/WorldPosReconstruction.hlsli"
#undef u_cameraInvPV

#include "Raster/Lighting/PBR/PBR_SpecularIbl.hlsli"

// Screen-space depth rim for Toon surfaces (Genshin-style; NoiRC256/URPSimpleGenshinShaders,
// kaze-mio/UnityGenshinToonShader). Offsets along the screen-projected normal and compares the
// GBuffer's linear camera distance; a neighbour that is farther by more than the threshold (or is
// sky) means this pixel sits on a silhouette. Uses the stored distance, not the reverse-Z depth,
// so the threshold is in metres. The width shrinks with distance to stay roughly world-sized.
float ToonScreenSpaceDepthRim(float3 worldPos, float3 N, uint2 pixel, float2 screenSize, float cameraDistance)
{
    const float4 clip0 = mul(float4(worldPos, 1.0), u_cameraPV);
    const float4 clip1 = mul(float4(worldPos + N * 0.05, 1.0), u_cameraPV);
    if (clip0.w <= 1e-5 || clip1.w <= 1e-5) {
        return 0.0;
    }
    // NDC y points up, pixel y points down.
    float2 dir = (clip1.xy / clip1.w) - (clip0.xy / clip0.w);
    dir.y = -dir.y;
    const float dirLen = length(dir);
    if (dirLen < 1e-6) {
        return 0.0;
    }
    dir /= dirLen;
    const float widthPx = clamp(u_toonParams3.y / max(cameraDistance, 0.25), 1.0, 32.0);
    const int2 samplePixel = clamp(int2(float2(pixel) + dir * widthPx + 0.5), int2(0, 0), int2(screenSize) - 1);
    const float neighbourDistance = GBufferNormalTex.Load(int3(samplePixel, 0)).w;
    if (neighbourDistance <= 0.0) {
        return 1.0;
    }
    return smoothstep(u_toonParams3.z, u_toonParams3.z * 2.0, neighbourDistance - cameraDistance);
}

// Front-hair shadow for toon surfaces: march from the pixel toward the light, project each step
// to the screen and accept only toon-hair GBuffer pixels as occluders. A step is occluded when the
// hair there is closer to the camera than the step point, by less than the thickness (so hair far
// in front does not shadow everything behind it). Returns 1 = lit, 0 = in hair shadow.
float ToonHairScreenSpaceShadow(float3 worldPos, float3 Ld, float2 screenSize)
{
    const int STEPS = 8;
    const float stepLength = u_toonHairShadow.x / STEPS;
    [loop]
    for (int i = 1; i <= STEPS; ++i) {
        const float3 p = worldPos + Ld * (stepLength * i);
        const float4 clip = mul(float4(p, 1.0), u_cameraPV);
        if (clip.w <= 1e-5) {
            return 1.0;
        }
        // NDC y points up, pixel y points down.
        const float2 ndc = clip.xy / clip.w;
        const float2 pixelPos = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * screenSize;
        if (any(pixelPos < 0.0) || any(pixelPos >= screenSize)) {
            return 1.0;
        }
        const int3 samplePixel = int3(pixelPos, 0);
        if (!IsToonHairShading(GBufferSpecularWorkflowTex.Load(samplePixel).a)) {
            continue;
        }
        const float hairDistance = GBufferNormalTex.Load(samplePixel).w;
        const float delta = length(p - u_cameraPos.xyz) - hairDistance;
        if (hairDistance > 0.0 && delta > 0.0 && delta < u_toonHairShadow.y) {
            return 0.0;
        }
    }
    return 1.0;
}

// Head sphere normals for toon surfaces: inside the head sphere, pull the normal toward the
// sphere normal so the face shades as one volume (proxy-shape normal edit, Motomura GDC 2015).
// The blend fades to zero over the outer 20% of the radius so the neck and collar do not pop.
float3 ToonHeadSphereNormal(float3 N, float3 worldPos)
{
    const float3 offset = worldPos - u_toonHeadSphere.xyz;
    const float radius = max(u_toonHeadSphere.w, 1e-4);
    const float distanceToCentre = length(offset);
    if (distanceToCentre >= radius || distanceToCentre < 1e-5) {
        return N;
    }
    const float weight = saturate(u_toonHeadNormal.x) * (1.0 - smoothstep(radius * 0.8, radius, distanceToCentre));
    return normalize(lerp(N, offset / distanceToCentre, weight));
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 screenSize = max(u_reflectionParams.zw, float2(1.0, 1.0));
    uint2 pixel = uint2(min(floor(input.pos.xy), screenSize - 1.0));
    float2 uv = saturate((float2(pixel) + 0.5) / screenSize);

    float depth = SceneDepthTex.Load(int3(pixel, 0)).r;
    float4 normalSample = GBufferNormalTex.Load(int3(pixel, 0));
    float cameraDistance = normalSample.w;
    // Reverse-Z: far plane is depth == 0.0, so background pixels satisfy depth <= 0.0.
    // cameraDistance is a linear distance (unaffected by reverse-Z), so its check is unchanged.
    if (depth <= 0.0 || cameraDistance <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 albedoSample = GBufferAlbedoTex.Load(int3(pixel, 0));
    float4 material = GBufferMaterialTex.Load(int3(pixel, 0));
    float4 emissiveSample = GBufferEmissiveTex.Load(int3(pixel, 0));
    float3 emissiveColor = emissiveSample.rgb;
    // 1 - HorizonOcclusion, computed in GBufferEncode.hlsli against the geometric normal.
    float horizon = 1.0 - saturate(emissiveSample.a);
    float4 specularWorkflow = GBufferSpecularWorkflowTex.Load(int3(pixel, 0));

    float3 albedo = saturate(albedoSample.rgb);
    float materialAlpha = saturate(albedoSample.a);
    float3 N = normalize(normalSample.xyz * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(u_cameraPos.xyz - worldPos);
    if (u_toonHeadNormal.w > 0.5 && IsToonShading(specularWorkflow.a) && !IsToonHairShading(specularWorkflow.a)) {
        N = ToonHeadSphereNormal(N, worldPos);
    }
    float NdotV = saturate(dot(N, V));

    float roughness = saturate(material.r);
    float metallic = saturate(material.g);
    float materialAo = saturate(material.b);
    float materialReflectionMask = saturate(material.a);
    bool useSpecularGlossiness = IsSpecularGlossinessWorkflow(specularWorkflow.a);
    float3 specularColor = saturate(specularWorkflow.rgb);

    // u_reflectionParams.zw is screen resolution in pixels (see LightCB.hlsli), not a
    // flag -- it is always > 0.5 for any real viewport, so this used to be a dead check.
    float runtimeAo = RuntimeAOTex.SampleLevel(LinearWrap, uv, 0).r;

    float rawAo = saturate(materialAo * runtimeAo);
    // UE-style MinOcclusion floor, applied once and used for both direct and indirect light.
    // Indirect used to take rawAo unfloored, so a fully AO-occluded pixel lost 100% of its
    // GI/IBL diffuse and specular while direct light stayed protected -- the black-edge
    // mismatch between the two contributions. Specular still gets its own relaxation on top,
    // in ComputeSpecularOcclusion.
    float ao = lerp(saturate(u_shadowParams.w), 1.0, rawAo);

    float3 F0 = useSpecularGlossiness
        ? specularColor
        : lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float specularEnergy = saturate(max(max(F0.r, F0.g), F0.b));
    float3 diffuseReflectance = useSpecularGlossiness
        ? albedo * (1.0 - specularEnergy)
        : albedo * (1.0 - metallic);

    float3 Lo = 0.0;
    float3 Ld = normalize(-u_dirDir.xyz);
    float3 H = normalize(V + Ld);
    float NdotL = saturate(dot(N, Ld));
    float directionalVisibility = 0.0;
    if (NdotL > 0.0) {
        float vis = ShadowVisibility(worldPos, N, Ld, NdotL);
        float contactVis = ComputeDirectionalContactShadow(worldPos, N, Ld);
        directionalVisibility = vis * contactVis;
    }

    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = roughness * roughness;
    float D = DistributionGGX(NdotH, a);
    float Vis = V_SmithGGXCorrelated(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);
    // Same multiple-scattering compensation as the IBL lobe (PBR_SpecularIbl.hlsli), so rough
    // metals do not lose energy under direct light that they keep under the environment.
    const float3 specularEnergyCompensation = SpecularEnergyCompensation(F0, NdotV, roughness);
    float3 spec = (D * Vis) * F * specularEnergyCompensation;
    float3 diffuse = (1.0 - F) * diffuseReflectance / 3.14159265;
    // Micro-shadowing on top of shadow-map visibility (Chan 2018, adopted by Filament).
    float dirMicroShadow = ComputeMicroShadowing(NdotL, ao, u_debugParams.y);

    const bool isToonSurface = IsToonShading(specularWorkflow.a);
    if (isToonSurface) {
        // Cel path: banded lambert, flat highlight, rim. Deliberately ignores D/G/F above --
        // see PBR_BRDF.hlsli for why quantizing the PBR result instead does not read as toon.
        // Every toon term is scaled by 1/pi, not only the diffuse: intensity pi then means "albedo as
        // painted" and each extra term adds its own parameter on top, as with a unit light in MToon /
        // UTS. Before, only the diffuse had the 1/pi, so specular 0.3 at intensity 2.97 added 0.89 of
        // white and the rim, hair bands and depth rim were pi times too strong as well.
        const float3 lightColor = u_dirColor.rgb * u_dirDir.w * (1.0 / 3.14159265);
        // SDF face shadow: the GBuffer stored the threshold-map lit factor in place of N.L.
        // Shadow-map visibility is only evaluated for N.L > 0, and a face normal facing away would
        // self-shadow, so on such pixels the face keeps only the map (no cast shadow, no micro shadow).
        const bool isToonFace = IsToonFaceShading(specularWorkflow.a);
        const float toonNdotL = isToonFace ? saturate(specularWorkflow.r) : NdotL;
        const bool isToonHair = IsToonHairShading(specularWorkflow.a);
        float hairShadow = 1.0;
        if (!isToonHair && u_toonHairShadow.w > 0.5) {
            hairShadow = lerp(1.0, ToonHairScreenSpaceShadow(worldPos, Ld, screenSize), saturate(u_toonHairShadow.z));
            directionalVisibility *= hairShadow;
        }
        const float toonVisibility = isToonFace
            ? (NdotL > 0.0 ? directionalVisibility : hairShadow)
            : directionalVisibility * dirMicroShadow;
        const float ramp = ToonDiffuseRamp(toonNdotL, toonVisibility,
                                           u_toonParams.x, u_toonParams.y, u_toonParams2.w);
        const float specGate = saturate(directionalVisibility) * step(1e-4, NdotL);
        const float rimTerm = ToonRimTerm(NdotV, NdotL, u_toonParams2.y);
        Lo += diffuseReflectance * ramp * lightColor
            + rimTerm * u_toonParams2.x * lightColor;
        if (isToonHair && u_toonHairStrength.w > 0.5) {
            // Angel ring: two thresholded Kajiya-Kay bands replace the flat highlight.
            // Primary in the light colour, secondary tinted by the albedo (Scheuermann 2004).
            const float3 T = ToonHairStrandTangent(N, normalize(u_toonHairUp.xyz));
            const float primary = ToonHairBand(T, N, H, u_toonHairShift.x, u_toonHairShift.z, u_toonHairStrength.z);
            const float secondary = ToonHairBand(T, N, H, u_toonHairShift.y, u_toonHairShift.w, u_toonHairStrength.z);
            Lo += (primary * u_toonHairStrength.x
                   + secondary * u_toonHairStrength.y * diffuseReflectance) * specGate * lightColor;
        } else {
            const float specTerm = ToonSpecularTerm(NdotH, u_toonParams.z, u_toonParams.w) * specGate;
            Lo += lerp(float3(1.0, 1.0, 1.0), specularColor, useSpecularGlossiness ? 1.0 : 0.0)
                  * specTerm * u_toonParams2.z * lightColor;
        }
        if (u_toonParams3.y > 0.0) {
            // Depth mask * Schlick-style fresnel pow(1 - N.V, 5), tinted by the albedo alone, as in
            // kaze-mio/UnityGenshinToonShader GetRim. Without the fresnel the whole silhouette band
            // got the same boost and, half white, read as a white halo around the character.
            const float depthRim = ToonScreenSpaceDepthRim(worldPos, N, pixel, screenSize, cameraDistance);
            const float rimFresnel = pow(saturate(1.0 - NdotV), 5.0);
            Lo += depthRim * rimFresnel * u_toonParams3.w * lightColor * diffuseReflectance;
        }
    } else {
        Lo += (diffuse + spec) * NdotL * (u_dirColor.rgb * u_dirDir.w) * directionalVisibility * dirMicroShadow;
    }

    // Point/spot direct lighting: identical math to the forward path (CookTorranceGGX_PS.hlsl).
    // See PBR_DirectLighting.hlsli for the shared implementation.
    PbrSurface surf;
    surf.worldPos = worldPos;
    surf.N = N;
    surf.V = V;
    surf.diffuseReflectance = diffuseReflectance;
    surf.F0 = F0;
    surf.roughness = roughness;
    surf.a = a;
    surf.NdotV = NdotV;
    surf.ao = ao;
    surf.specularEnergyCompensation = specularEnergyCompensation;
    Lo += AccumulatePointLights(surf, u_debugParams.y);
    Lo += AccumulateSpotLights(surf, u_debugParams.y);

    float3 indirectDiffuse = 0.0;
    float3 Fibl = FresnelSchlick(NdotV, F0);
    float3 kdIbl = (1.0 - Fibl);
    if (g_giEnabled > 0.5) {
        // Lambertian BRDF: outgoing radiance is E * albedo / PI. GI_SampleProbeGrid returns
        // irradiance E -- its SH is convolved with the cosine lobe (A0=PI, A1=2PI/3,
        // A2=PI/4) -- so the 1/PI still belongs here, exactly like the direct-light
        // diffuse term above. Without it the indirect diffuse is PI times too bright,
        // which reads as up-facing surfaces (floors, roofs) being blown out.
        float3 probeIrradiance = GI_SampleProbeGrid(worldPos, N);
        indirectDiffuse = kdIbl * (probeIrradiance / 3.14159265) * diffuseReflectance * ao;
    } else if (u_iblParams.x > 0.5) {
        // EvaluateDiffuseIrradianceFromSh returns irradiance E (same cosine-lobe
        // convolution as the probe SH) and needs the Lambert 1/PI. The prefiltered
        // cubemap instead stores a cosine-weighted average radiance (E/PI already),
        // so only the SH variant is divided here.
        float3 iblDiffuse = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N) / 3.14159265
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        indirectDiffuse = kdIbl * iblDiffuse * diffuseReflectance * ao * u_iblParams.y;
    }

    // Specular occlusion reuses the floored AO term above;
    // ComputeSpecularOcclusion (PBR_BRDF.hlsli) blends the SpecularOcclusion curve with
    // ao^2 by metallic and folds in HorizonOcclusion, matching the forward path
    // (CookTorranceGGX_PS.hlsl) so the same surface is not occluded differently depending
    // on which pass shades it.
    float specularOcclusion = ComputeSpecularOcclusionWithHorizon(ao, NdotV, roughness, metallic, horizon);
    // Fade out by however much the reflection composite pass will add for this pixel, so the
    // traced/screen-space reflection replaces the IBL specular instead of stacking on it. A
    // miss (alpha 0) leaves the IBL specular untouched.
    float reflectionCoverage = 0.0;
    if (u_reflectionParams.x > 0.5 && materialReflectionMask > 0.0) {
        const float reflectionAlpha = ReflectionTex.SampleLevel(LinearWrap, uv, 0).a;
        reflectionCoverage = ReflectionCoverage(reflectionAlpha, u_reflectionParams.y,
                                                materialReflectionMask, roughness);
    }
    float3 indirectSpecular = EvaluateSpecularIbl(N, V, F0, roughness, specularOcclusion) *
                              (1.0 - reflectionCoverage);

    if (isToonSurface) {
        // Cel surfaces take a reduced, flat ambient and no environment specular at all.
        //
        // Measured before this: every toon surface clipped to ~195/255 with its lit and shadow
        // bands within 0.3/255 of each other -- the banding was intact in Lo but buried under a
        // full-strength IBL term, and the saturation dragged the base colour to neutral grey.
        // EvaluateSpecularIbl is gated only on the IBL flag, not on the material's reflection
        // strength, so a cel material could not opt out of the sheen by itself.
        indirectDiffuse *= u_toonParams3.x;
        indirectSpecular = (float3)0;
    }

    // Emissive is self-lit, so AO must not darken it. The old 0.01 * albedo * ao constant ambient
    // double-counted with the IBL/GI terms and is gone.
    float3 color = indirectDiffuse + indirectSpecular + Lo + emissiveColor;

    return float4(color, materialAlpha);
}
