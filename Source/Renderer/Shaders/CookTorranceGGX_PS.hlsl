cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
    float4 u_materialBaseColor;
    float4 u_materialEmissiveRoughness;
    float4 u_materialParams;
    float4 u_materialSpecularWorkflow; // rgb: specular color, w: 0=metallic-roughness, 1=specular-glossiness
}

Texture2D AlbedoTex    : register(t0);
Texture2DArray ShadowMapTex : register(t1);
TextureCube IrradianceTex    : register(t4);
TextureCube PrefilterTex     : register(t5);
Texture2D BrdfLutTex         : register(t6);
Texture2D OcclusionTex       : register(t7);
Texture2D ReflectionTex      : register(t8);
Texture2D RuntimeAOTex       : register(t9);
Texture2D SceneDepthTex      : register(t11);
Texture2D SpotShadowMapTex   : register(t12);
SamplerState LinearWrap : register(s0);

#include "Common/LightCB.hlsli"
#include "GI/GI_Common.hlsli"

struct PointLight
{
    float4 posRange;
    float4 colorIntensity;
};

struct SpotLight
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    float4 params; // x: cosOuter
};

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

float3 EvaluateDiffuseIrradianceFromSh(float3 n)
{
    const float x = n.x;
    const float y = n.y;
    const float z = n.z;

    // Real SH basis (L2), multiplied by cosine-kernel convolution factor per band.
    const float b0 = 0.282095 * 3.14159265;
    const float b1 = (0.488603 * y) * 2.09439510;
    const float b2 = (0.488603 * z) * 2.09439510;
    const float b3 = (0.488603 * x) * 2.09439510;
    const float b4 = (1.092548 * x * y) * 0.78539816;
    const float b5 = (1.092548 * y * z) * 0.78539816;
    const float b6 = (0.315392 * (3.0 * z * z - 1.0)) * 0.78539816;
    const float b7 = (1.092548 * x * z) * 0.78539816;
    const float b8 = (0.546274 * (x * x - y * y)) * 0.78539816;

    float3 irradiance =
        u_diffuseSh[0].rgb * b0 +
        u_diffuseSh[1].rgb * b1 +
        u_diffuseSh[2].rgb * b2 +
        u_diffuseSh[3].rgb * b3 +
        u_diffuseSh[4].rgb * b4 +
        u_diffuseSh[5].rgb * b5 +
        u_diffuseSh[6].rgb * b6 +
        u_diffuseSh[7].rgb * b7 +
        u_diffuseSh[8].rgb * b8;

    return max(irradiance, 0.0);
}

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

float ShadowVisibility(float3 worldPos, float3 normal, float3 lightDir, float NdotL)
{
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

struct PSOutput
{
    float4 color    : SV_TARGET0; // SceneColor    — final lit output
    float4 albedo   : SV_TARGET1; // GBufferAlbedo — base color RGB + alpha
    float4 normal   : SV_TARGET2; // GBufferNormal — world-space normal XYZ (encoded 0-1) + camera distance
    float4 material : SV_TARGET3; // GBufferMaterial — roughness(R) metallic(G) AO(B) 0(A)
    float4 emissive : SV_TARGET4; // GBufferEmissive — emissive color RGB + 0
};

PSOutput PSMain(PSInput i)
{
    float3 albedo = AlbedoTex.Sample(LinearWrap, i.uv).rgb * i.color.rgb * u_materialBaseColor.rgb;

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
    if (!useSpecularGlossiness && u_materialParams.w > 0.5) {
        roughness = saturate(roughness * materialTextureSample.g);
        metallic = saturate(metallic * materialTextureSample.b);
    }
    const int aoMode = (int)(u_materialParams.z + 0.5);
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
    float diffuseEnergyScale = useSpecularGlossiness
        ? saturate(1.0 - max(max(F0.r, F0.g), F0.b))
        : (1.0 - metallic);
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
    float roughnessAtten = 0.0;
    bool allowSpecularIblFallback = false;
    bool useSwrtReflection = false;
    float specularAo = 1.0;
    if (u_reflectionParams.x > 0.5 && u_reflectionParams.z > 0.5 && u_reflectionParams.w > 0.5) {
        if (u_reflectionParams.x > 1.5) {
            float2 reflectionUv = 0.0;
            bool allowSkyIblMiss = false;
            float3 reflectionDir = reflect(-V, N);
            if (TraceScreenSpaceReflection(i.worldPos, N, reflectionDir, roughness, reflectionUv, allowSkyIblMiss)) {
                localReflection = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0).rgb;
                roughnessAtten = saturate(u_reflectionParams.y) * lerp(1.0, 0.35, roughness);
            } else {
                allowSpecularIblFallback = allowSkyIblMiss;
            }
        } else {
            float2 reflectionUv = saturate(i.position.xy / u_reflectionParams.zw);
            float4 reflectionSample = ReflectionTex.SampleLevel(LinearWrap, reflectionUv, 0);
            localReflection = reflectionSample.rgb;
            // SWRT is generated after this lighting pass in the current render graph.
            // FinalLit receives the current-frame result in the post reflection composite.
            // Debug views below still sample ReflectionTex directly.
            roughnessAtten = 0.0;
            useSwrtReflection = false;
        }
    }
    if (u_iblParams.x > 0.5) {
        // Split-sum approximation for specular IBL:
        // L_ibl = kD * (irradiance * albedo) + prefilteredEnv(R, roughness) * (F0 * brdf.x + brdf.y)
        float3 Fibl = FresnelSchlick(saturate(dot(N, V)), F0);
        float3 kdIbl = (1.0 - Fibl);
        // Probe GI irradiance (replaces IBL diffuse when GI is enabled)
        float3 probeIrradiance = GI_SampleProbeGrid(i.worldPos, N);
        float3 iblIrradiance = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N)
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        float3 irradiance = (g_giEnabled > 0.5) ? probeIrradiance : iblIrradiance;
        float3 diffuseIBL = irradiance * diffuseReflectance;

        float3 R = reflect(-V, N);
        float3 prefiltered = PrefilterTex.SampleLevel(LinearWrap, R, roughness * u_iblParams.z).rgb;
        float2 envBrdf = BrdfLutTex.SampleLevel(LinearWrap, float2(saturate(dot(N, V)), roughness), 0).rg;
        float3 specIBL = prefiltered * (F0 * envBrdf.x + envBrdf.y);

        // AO-driven occlusion for indirect light:
        // - Diffuse IBL uses linear AO.
        // - Specular AO is relaxed for metallic surfaces so they keep a visible
        //   reflection response instead of disappearing under occlusion.
        specularAo = lerp(rawAo * rawAo,
                          saturate(SpecularOcclusion(rawAo, NdotV, roughness)),
                          saturate(metallic));
        float3 indirectDiffuse = kdIbl * diffuseIBL * rawAo * u_iblParams.y;
        // Specular IBL is the fallback for untraced reflection rays. When SWRT
        // reflection is present, its RGB already encodes either traced-hit radiance
        // or miss-time environment radiance, so do not add an unconditional IBL floor.
        float3 indirectSpecular = (roughnessAtten > 0.0)
            ? 0.0
            : (allowSpecularIblFallback ? specIBL * specularAo * u_iblParams.y : 0.0);
        if (roughnessAtten > 0.0) {
            if (useSwrtReflection) {
                // Apply the current surface's split-sum specular BRDF to the traced
                // incoming radiance. This keeps SWRT in the same linear-lighting path
                // as regular specular IBL instead of additively compositing final color.
                float3 reflectionBrdf = F0 * envBrdf.x + envBrdf.y;
                indirectSpecular = localReflection * reflectionBrdf * roughnessAtten * specularAo;
            } else {
                // SSR samples screen color, so keep the existing Fresnel-style weight.
                float3 F_refl = FresnelSchlick(saturate(NdotV), F0);
                float smoothness = 1.0 - roughness;
                float dielectricFloor = 0.20 * smoothness * smoothness;
                float3 visibleReflectionFresnel = lerp(
                    max(F_refl, float3(dielectricFloor, dielectricFloor, dielectricFloor)),
                    F_refl,
                    saturate(metallic));
                float visibleReflectionAo = max(specularAo, 0.35);
                indirectSpecular = visibleReflectionFresnel * localReflection * roughnessAtten * visibleReflectionAo;
            }
        }
        ambient = indirectDiffuse + indirectSpecular;
    } else if (roughnessAtten > 0.0) {
        float3 F_refl = FresnelSchlick(saturate(NdotV), F0);
        ambient += localReflection * F_refl * roughnessAtten * (rawAo * rawAo);
    }
    // Ambient floor: even in fully-occluded areas, retain a minimum bounce-light contribution
    // so indoor surfaces never go completely black. 0.04 ≈ dark interior scatter floor.
    static const float kAmbientFloor = 0.01;
    ambient += kAmbientFloor * albedo * ao;

    float3 emissiveColor = u_materialEmissiveRoughness.rgb;
    float3 color = ambient + Lo + emissiveColor;

    PSOutput o;
    o.albedo   = float4(saturate(albedo), 1.0);
    o.normal   = float4(N * 0.5 + 0.5, length(i.worldPos - u_cameraPos.xyz)); // W = linear camera distance for SWRT world-pos reconstruction
    o.material = float4(roughness, metallic, ao, 0.0);
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
    if (debugMode == 13) { // Reflection alpha / roughness attenuation
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

    // Output transform:
    // Reinhard tone map: c' = c / (1 + c)
    // Gamma encode (approx sRGB): c_out = c'^(1/2.2)
    color = color / (color + 1.0);
    color = pow(color, 1.0/2.2);
    o.color = float4(color, 1.0);
    return o;
}
