cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
    float4 u_materialSpecularWorkflow; // rgb: specular color, w: 0=metallic-roughness, 1=specular-glossiness
    float4 u_materialReflectionParams; // x: authored reflection strength, y: transmission, z: ior, w: thickness
    float4 u_materialVolumeParams; // rgb: attenuation color, w: attenuation distance
    float4 u_materialTransparencyParams; // x: transparent shell strength
}

Texture2D AlbedoTex    : register(t0);
Texture2DArray ShadowMapTex : register(t1);
TextureCube IrradianceTex    : register(t4);
Texture2D OcclusionTex       : register(t7);
Texture2D ReflectionTex      : register(t8);
Texture2D RuntimeAOTex       : register(t9);
Texture2D SceneDepthTex      : register(t11);
Texture2D SpotShadowMapTex   : register(t12);
Texture2DArray<float2> ShadowVSMTex : register(t13);
Texture2D<float> TransparentBackfaceDistanceTex : register(t14);
SamplerState LinearWrap : register(s0);

#include "Common/LightCB.hlsli"
#include "GI/GI_Common.hlsli"
#include "PBR_LightTypes.hlsli"

StructuredBuffer<PointLight> u_pointLights : register(t2);
StructuredBuffer<SpotLight> u_spotLights : register(t3);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

#include "PBR_BRDF.hlsli"
#include "PBR_IBL.hlsli"
#include "PBR_Shadow.hlsli"
#include "PBR_ScreenReflection.hlsli"

struct PSOutput
{
    float4 color    : SV_TARGET0; // SceneColor    — final lit output
    float4 albedo   : SV_TARGET1; // GBufferAlbedo — base color RGB + alpha
    float4 normal   : SV_TARGET2; // GBufferNormal — world-space normal XYZ (encoded 0-1) + camera distance
    float4 material : SV_TARGET3; // GBufferMaterial - roughness(R) metallic(G) iblVisibility(B) reflectionMask(A)
    float4 emissive : SV_TARGET4; // GBufferEmissive — emissive color RGB + 0
};

PSOutput PSMain(PSInput i)
{
    float4 albedoSample = AlbedoTex.Sample(LinearWrap, i.uv);
    float3 albedo = albedoSample.rgb * i.color.rgb * u_materialBaseColor.rgb;
    float materialAlpha = saturate(albedoSample.a * i.color.a * u_materialBaseColor.a);

    const float4 materialTextureSample = OcclusionTex.Sample(LinearWrap, i.uv);
    const float aoSample = saturate(materialTextureSample.r);
    const float occlusionStrength = saturate(u_materialParams.y);
    float materialAo = lerp(1.0, aoSample, occlusionStrength);
    float runtimeAo = 1.0;

    // Runtime-generated AO (SSAO/RTAO) is bound at slot 8, register t9.
    // u_reflectionParams.zw = screen width/height (same dimensions as the AO/reflection textures).
    if (u_reflectionParams.z > 0.5 && u_reflectionParams.w > 0.5)
    {
        float2 screenUV = i.position.xy / u_reflectionParams.zw;
        runtimeAo = RuntimeAOTex.SampleLevel(LinearWrap, screenUV, 0).r;
    }
    const bool useSpecularGlossiness = (u_materialSpecularWorkflow.w > 0.5);
    float metallic = useSpecularGlossiness ? 0.0 : saturate(u_materialParams.x);
    float roughness = saturate(u_materialEmissiveRoughness.w);
    float transmission = saturate(u_materialReflectionParams.y);
    float materialIor = max(u_materialReflectionParams.z, 1.0);
    float materialThickness = max(u_materialReflectionParams.w, 0.0);
    float3 attenuationColor = saturate(u_materialVolumeParams.rgb);
    float attenuationDistance = max(u_materialVolumeParams.w, 1e-4);
    if (!useSpecularGlossiness && u_materialParams.w > 0.5) {
        roughness = saturate(roughness * materialTextureSample.g);
        metallic = saturate(metallic * materialTextureSample.b);
    }
    const int aoMode = (int)(u_materialParams.z + 0.5);
    if (aoMode == 0) {
        runtimeAo = 1.0;
    }
    float ao = 1.0;
    if (aoMode == 0) {
        ao = materialAo;
    } else if (aoMode == 1) {
        ao = runtimeAo;
    } else if (aoMode == 2) {
        ao = runtimeAo;
    } else {
        ao = materialAo * runtimeAo;
    }
    const float rawAo = saturate(ao);
    const float iblVisibility = saturate(materialAo * runtimeAo);
    // UE-style MinOcclusion: remap AO from [0,1] to [minOcc,1] so fully-occluded
    // areas never go completely dark.  u_shadowParams.w carries the floor value.
    // lerp(minOcc, 1.0, ao) = minOcc + (1-minOcc)*ao — equivalent to UE MinOcclusion.
    {
        float minOcc = saturate(u_shadowParams.w);
        ao = minOcc + (1.0 - minOcc) * rawAo;
    }
    float3 N = normalize(i.worldN);
    float3 Vvec = u_cameraPos.xyz - i.worldPos;
    float vLen = length(Vvec);
    float3 V = (vLen > 1e-5) ? (Vvec / vLen) : float3(0.0, 0.0, 1.0);

    float3 F0 = useSpecularGlossiness
        ? saturate(u_materialSpecularWorkflow.rgb)
        : lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const bool reflectionModeEnabled = (u_reflectionParams.x > 0.5);
    float authoredReflectionMask = ReflectionMaterialMask(u_materialReflectionParams.x);
    float physicalReflectionMask = useSpecularGlossiness
        ? saturate(max(max(F0.r, F0.g), F0.b) * (1.0 - roughness))
        : saturate(metallic * (1.0 - roughness));
    float materialReflectionMask = max(authoredReflectionMask, physicalReflectionMask);
    // glTF transmission affects only dielectric energy. Metals absorb transmitted light.
    float effectiveTransmission = transmission * (1.0 - metallic);
    float diffuseEnergyScale = useSpecularGlossiness
        ? saturate(1.0 - max(max(F0.r, F0.g), F0.b))
        : (1.0 - metallic);
    diffuseEnergyScale *= (1.0 - effectiveTransmission);
    float3 diffuseReflectance = albedo * diffuseEnergyScale;

    // Directional light (shadowed)
    // Cook-Torrance BRDF:
    // f_r = (kD * albedo / PI) + (D * F * G) / (4 * (N.V) * (N.L))
    // Lo += f_r * radiance * (N.L) * visibility
    float3 Ld = normalize(-u_dirDir.xyz);
    float3 H = normalize(V + Ld);
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, Ld));
    float vis = ShadowVisibility(i.worldPos, N, Ld, NdotL);
    float contactVis = ComputeDirectionalContactShadow(i.worldPos, N, Ld);
    float3 Lo = 0.0;
    float directionalVisibility = (NdotL > 0.0) ? (vis * contactVis) : 0.0;
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = roughness * roughness;
    float D = DistributionGGX(NdotH, a);
    float k = (roughness + 1.0);
    k = (k * k) / 8.0; // Schlick-GGX remapping
    float G = GeometrySmith(NdotV, NdotL, k);
    float3 F = FresnelSchlick(VdotH, F0);
    float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    // Energy split: specular-glossiness limits diffuse by max(F0), while
    // metallic-roughness limits diffuse by (1-metallic).
    float3 kd = (1.0 - F);
    // Lambert diffuse: f_d = energy-limited diffuse reflectance / PI
    float3 diffuse = kd * diffuseReflectance / 3.14159265;
    float3 dirColor = u_dirColor.rgb * u_dirDir.w;
    Lo += (diffuse + spec) * NdotL * dirColor * directionalVisibility;

    // Point lights (no shadow)
    // Distance attenuation (authoring-friendly polynomial):
    // atten = saturate(1 - d / range)^2
    int pointCount = (int)u_lightCounts.x;
    for (int li = 0; li < pointCount; ++li) {
        PointLight pl = u_pointLights[li];
        float4 posRange = pl.posRange;
        float4 colInt = pl.colorIntensity;
        float3 toL = posRange.xyz - i.worldPos;
        float dist = length(toL);
        if (dist < posRange.w && dist > 1e-4 && colInt.w > 0.0) {
            float3 Lp = toL / dist;
            float atten = saturate(1.0 - dist / max(posRange.w, 1e-3));
            atten *= atten;
            float3 Hp = normalize(V + Lp);
            float NdotLp = saturate(dot(N, Lp));
            float NdotHp = saturate(dot(N, Hp));
            float VdotHp = saturate(dot(V, Hp));
            float Dp = DistributionGGX(NdotHp, a);
            float Gp = GeometrySmith(NdotV, NdotLp, k);
            float3 Fp = FresnelSchlick(VdotHp, F0);
            float3 specP = (Dp * Gp) * Fp / max(4.0 * NdotV * NdotLp, 1e-4);
            float3 kdP = (1.0 - Fp);
            float3 diffP = kdP * diffuseReflectance / 3.14159265;
            float3 pointColor = colInt.rgb * colInt.w * atten;
            Lo += (diffP + specP) * NdotLp * pointColor;
        }
    }

    // Spot lights with shadow (li==0 samples SpotShadowMapTex at t12)
    // Cone attenuation:
    // spot = smoothstep(cosOuter, cosInner, cosTheta)
    int spotCount = (int)u_lightCounts.y;
    for (int li = 0; li < spotCount; ++li) {
        SpotLight sl = u_spotLights[li];
        float4 posRange = sl.posRange;
        float4 dirInner = sl.dirCosInner;
        float4 colInt = sl.colorIntensity;
        float4 params = sl.params;
        float3 toL = posRange.xyz - i.worldPos;
        float dist = length(toL);
        if (dist < posRange.w && dist > 1e-4 && colInt.w > 0.0) {
            float3 Ls = toL / dist;
            float cosTheta = dot(normalize(-Ls), normalize(dirInner.xyz));
            float spot = smoothstep(params.x, dirInner.w, cosTheta);
            float atten = saturate(1.0 - dist / max(posRange.w, 1e-3));
            atten *= atten;
            float3 Hs = normalize(V + Ls);
            float NdotLs = saturate(dot(N, Ls));
            float NdotHs = saturate(dot(N, Hs));
            float VdotHs = saturate(dot(V, Hs));
            float Ds = DistributionGGX(NdotHs, a);
            float Gs = GeometrySmith(NdotV, NdotLs, k);
            float3 Fs = FresnelSchlick(VdotHs, F0);
            float3 specS = (Ds * Gs) * Fs / max(4.0 * NdotV * NdotLs, 1e-4);
            float3 kdS = (1.0 - Fs);
            float3 diffS = kdS * diffuseReflectance / 3.14159265;

            // Spot shadow: 3x3 PCF for li==0 when shadow is enabled
            float spotShadow = 1.0;
            if (li == 0 && u_spotShadowParams.z > 0.5)
            {
                float4 sc = mul(float4(i.worldPos, 1.0), u_spotLightVP);
                sc.xyz /= sc.w;
                float2 shadowUV = sc.xy * 0.5 + 0.5;
                shadowUV.y = 1.0 - shadowUV.y;
                float shadowDepth = sc.z - u_spotShadowParams.x; // depth bias
                if (all(shadowUV >= 0.0) && all(shadowUV <= 1.0) && sc.w > 0.0)
                {
                    float texelSize = 1.0 / u_spotShadowParams.w;
                    float shadowSum = 0.0;
                    [unroll] for (int sy = -1; sy <= 1; ++sy)
                    [unroll] for (int sx = -1; sx <= 1; ++sx)
                    {
                        float sampleD = SpotShadowMapTex.Sample(LinearWrap,
                            shadowUV + float2(sx, sy) * texelSize).r;
                        shadowSum += (sampleD >= shadowDepth) ? 1.0 : 0.0;
                    }
                    spotShadow = shadowSum / 9.0;
                }
            }

            float3 spotColor = colInt.rgb * colInt.w * atten * spot * spotShadow;
            Lo += (diffS + specS) * NdotLs * spotColor;
        }
    }

    float3 ambient = 0.03 * albedo * ao;
    float3 localReflection = 0.0;
    float reflectionWeight = 0.0;
    float specularAo = 1.0;
    if (reflectionModeEnabled && materialReflectionMask > 0.0 &&
        u_reflectionParams.z > 0.5 && u_reflectionParams.w > 0.5) {
        if (u_reflectionParams.x > 1.5) {
            float2 reflectionUv = 0.0;
            bool allowSkyIblMiss = false;
            float3 reflectionDir = reflect(-V, N);
            if (TraceScreenSpaceReflection(i.worldPos, N, reflectionDir, roughness, reflectionUv, allowSkyIblMiss)) {
                localReflection = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0).rgb;
                reflectionWeight = saturate(u_reflectionParams.y) * lerp(1.0, 0.35, roughness);
            }
        } else {
            float2 reflectionUv = saturate(i.position.xy / u_reflectionParams.zw);
            float4 reflectionSample = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0);
            localReflection = reflectionSample.rgb;
            // SWRT reflections need the GBuffer produced by this lighting pass.
            // FinalLit receives them through the immediate reflection composite.
            reflectionWeight = 0.0;
        }
    }
    float3 indirectDiffuse = 0.0;
    float3 fallbackSpecular = 0.0;
    if (u_iblParams.x > 0.5) {
        // IBL contributes only diffuse/environment lighting here. Specular
        // reflections come from SSR/SWRT on materials that explicitly opt in.
        float3 Fibl = FresnelSchlick(saturate(dot(N, V)), F0);
        float3 kdIbl = (1.0 - Fibl);
        // Probe GI irradiance (replaces IBL diffuse when GI is enabled)
        float3 probeIrradiance = GI_SampleProbeGrid(i.worldPos, N);
        float3 iblIrradiance = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N)
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        float3 irradiance = (g_giEnabled > 0.5) ? probeIrradiance : iblIrradiance;
        float3 diffuseIBL = irradiance * diffuseReflectance;

        // AO-driven occlusion for indirect light:
        // - Diffuse IBL uses linear AO.
        // - Specular AO is relaxed for metallic surfaces so they keep a visible
        //   reflection response instead of disappearing under occlusion.
        specularAo = lerp(iblVisibility * iblVisibility,
                          saturate(SpecularOcclusion(iblVisibility, NdotV, roughness)),
                          saturate(metallic));
        indirectDiffuse = kdIbl * diffuseIBL * iblVisibility * u_iblParams.y;
    }

    // Reflection hierarchy is independent from the IBL toggle:
    // - SWRT/SSR supply the reflected radiance when they have data.
    // - Misses stay black; Skybox/IBL is not used as reflected radiance.
    if (reflectionWeight > 0.0) {
        float3 reflectionFresnel = FresnelSchlick(saturate(NdotV), F0);
        float smoothness = 1.0 - roughness;
        float dielectricFloor = 0.20 * smoothness * smoothness;
        reflectionFresnel = lerp(
            max(reflectionFresnel, float3(dielectricFloor, dielectricFloor, dielectricFloor)),
            reflectionFresnel,
            saturate(metallic));
        float3 reflectionSpecular = localReflection * reflectionFresnel * specularAo;
        ambient = indirectDiffuse + lerp(fallbackSpecular, reflectionSpecular, saturate(reflectionWeight));
    } else {
        ambient = indirectDiffuse + fallbackSpecular;
    }
    // Ambient floor: even in fully-occluded areas, retain a minimum bounce-light contribution
    // so indoor surfaces never go completely black. 0.04 ≈ dark interior scatter floor.
    static const float kAmbientFloor = 0.01;
    ambient += kAmbientFloor * albedo * ao;

    float3 emissiveColor = u_materialEmissiveRoughness.rgb;
    float3 color = ambient + Lo + emissiveColor;
    float outputAlpha = materialAlpha;
    if (effectiveTransmission > 0.001) {
        const bool hasSceneColorCopy =
            (u_reflectionParams.z > 0.5) &&
            (u_reflectionParams.w > 0.5);
        const float3 viewFresnel = FresnelSchlick(NdotV, F0);
        const float transmissionWeight =
            effectiveTransmission * (1.0 - saturate(max(max(viewFresnel.r, viewFresnel.g), viewFresnel.b)));

        if (hasSceneColorCopy) {
            float2 screenUv = saturate(i.position.xy / u_reflectionParams.zw);
            float refractionScale = saturate(materialIor - 1.0) * 0.035 * (1.0 - roughness);
            float2 refractionUv = saturate(screenUv + N.xy * refractionScale);
            float3 surfaceRadiance = color;
            float3 transmittedRadiance = ReflectionTex.SampleLevel(LinearWrap, refractionUv, 0).rgb * albedo;
            float effectiveThickness = materialThickness;
            float backfaceDistance = TransparentBackfaceDistanceTex.SampleLevel(LinearWrap, screenUv, 0);
            if (backfaceDistance > vLen) {
                effectiveThickness = max(effectiveThickness, backfaceDistance - vLen);
            }
            if (effectiveThickness > 0.0) {
                float3 volumeTransmittance =
                    pow(max(attenuationColor, float3(0.001, 0.001, 0.001)),
                        effectiveThickness / attenuationDistance);
                transmittedRadiance *= volumeTransmittance;
            }

            const float fresnelStrength = saturate(max(max(viewFresnel.r, viewFresnel.g), viewFresnel.b));
            const float surfaceVisibility = lerp(0.28, 1.0, fresnelStrength);
            const float3 visibleSurface = surfaceRadiance * surfaceVisibility;
            color = lerp(surfaceRadiance, transmittedRadiance + visibleSurface, transmissionWeight);
        }

        // Authored alpha is coverage/opacity. Transmission changes RGB by
        // sampling the scene behind the surface; it must not erase alpha again.
        outputAlpha = materialAlpha;
    }

    PSOutput o;
    o.albedo   = float4(saturate(albedo), materialAlpha);
    o.normal   = float4(N * 0.5 + 0.5, length(i.worldPos - u_cameraPos.xyz)); // W = linear camera distance for SWRT world-pos reconstruction
    o.material = float4(roughness, metallic, iblVisibility, materialReflectionMask);
    o.emissive = float4(emissiveColor, 0.0);

    const int debugMode = (int)(u_debugParams.x + 0.5);
    if (debugMode == 1) { // Albedo
        o.color = float4(saturate(albedo), 1.0);
        return o;
    }
    if (debugMode == 2) { // Normal
        o.color = float4(N * 0.5 + 0.5, 1.0);
        return o;
    }
    if (debugMode == 3) { // Roughness
        o.color = float4(roughness.xxx, 1.0);
        return o;
    }
    if (debugMode == 4) { // Metallic
        o.color = float4(metallic.xxx, 1.0);
        return o;
    }
    if (debugMode == 5) { // Ambient occlusion
        o.color = float4(ao.xxx, 1.0);
        return o;
    }
    if (debugMode == 6) { // Directional light visibility
        o.color = float4(directionalVisibility.xxx, 1.0);
        return o;
    }
    if (debugMode == 7) { // Emissive
        o.color = float4(emissiveColor, 1.0);
        return o;
    }
    if (debugMode == 8) { // Runtime AO raw
        o.color = float4(runtimeAo.xxx, 1.0);
        return o;
    }
    if (debugMode == 9) { // Runtime AO filtered
        o.color = float4(runtimeAo.xxx, 1.0);
        return o;
    }
    if (debugMode == 10) { // Directional light direction
        o.color = float4(Ld * 0.5 + 0.5, 1.0);
        return o;
    }
    if (debugMode == 11) { // Directional NdotL
        o.color = float4(NdotL.xxx, 1.0);
        return o;
    }
    if (debugMode == 12) { // Reflection radiance
        float2 reflectionUv = saturate(i.position.xy / max(u_reflectionParams.zw, float2(1.0, 1.0)));
        float4 reflectionSample = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0);
        o.color = float4(saturate(reflectionSample.rgb), 1.0);
        return o;
    }
    if (debugMode == 13) { // Reflection alpha / radiance confidence
        float2 reflectionUv = saturate(i.position.xy / max(u_reflectionParams.zw, float2(1.0, 1.0)));
        float reflectionAlpha = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0).a;
        o.color = float4(saturate(reflectionAlpha).xxx, 1.0);
        return o;
    }
    if (debugMode == 14) { // SWRT reflection hit distance
        float2 reflectionUv = saturate(i.position.xy / max(u_reflectionParams.zw, float2(1.0, 1.0)));
        float hitDistance = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0).r;
        o.color = float4(saturate(hitDistance).xxx, 1.0);
        return o;
    }
    if (debugMode == 15) { // SWRT reflection composite only
        o.color = float4(0.0, 0.0, 0.0, 1.0);
        return o;
    }

    o.color = float4(color, outputAlpha);
    return o;
}
