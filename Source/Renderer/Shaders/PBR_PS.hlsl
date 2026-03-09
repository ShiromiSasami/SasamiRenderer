Texture2D AlbedoTex    : register(t0);
Texture2D ShadowMapTex : register(t1);
TextureCube IrradianceTex : register(t4);
TextureCube PrefilterTex  : register(t5);
Texture2D BrdfLutTex      : register(t6);
Texture2D OcclusionTex    : register(t7);
SamplerState LinearWrap : register(s0);

#include "Common/LightCB.hlsli"

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

float ShadowVisibility(float4 lightPos)
{
    float3 sc = lightPos.xyz / max(lightPos.w, 1e-6);
    float2 suv = sc.xy * 0.5 + 0.5;   // NDC->UV
    float  sdepth = sc.z * 0.5 + 0.5; // [-1,1] -> [0,1]
    if (any(suv < 0.0) || any(suv > 1.0)) return 1.0;

    float bias = 0.0015; // simple constant bias
    float2 texel = float2(1.0 / 1024.0, 1.0 / 1024.0);
    float acc = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float2 uv = suv + float2(dx, dy) * texel;
            float sd = ShadowMapTex.SampleLevel(LinearWrap, uv, 0).r;
            acc += (sdepth - bias <= sd) ? 1.0 : 0.0;
        }
    }
    // visibility = (1 / 9) * sum_{kernel} step(shadowDepth >= receiverDepth - bias)
    return acc / 9.0;
}

float4 PSMain(PSInput i) : SV_TARGET
{
    // Albedo in sRGB assumed; if not sRGB-corrected by sampler state, apply manual gamma.
    float3 albedo = AlbedoTex.Sample(LinearWrap, i.uv).rgb;

    // Material ambient occlusion (glTF occlusionTexture uses R channel).
    // AO in [0,1]: 0=fully occluded, 1=no occlusion.
    float ao = saturate(OcclusionTex.Sample(LinearWrap, i.uv).r);

    // Minimal PBR params (constants for now)
    float metallic = 0.0;
    float roughness = 0.5; // perceptual roughness [0..1]
    float3 N = normalize(i.worldN);
    float3 Vvec = u_cameraPos.xyz - i.worldPos;
    float vLen = length(Vvec);
    float3 V = (vLen > 1e-5) ? (Vvec / vLen) : float3(0.0, 0.0, 1.0);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 Lo = 0.0;
    float vis = ShadowVisibility(i.lightPos);

    // Directional light (shadowed)
    // Cook-Torrance BRDF:
    // f_r = (kD * albedo / PI) + (D * F * G) / (4 * (N.V) * (N.L))
    // Lo += f_r * radiance * (N.L) * visibility
    float3 Ld = normalize(-u_dirDir.xyz);
    float3 H = normalize(V + Ld);
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, Ld));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = roughness * roughness;
    float D = DistributionGGX(NdotH, a);
    float k = (roughness + 1.0);
    k = (k * k) / 8.0; // Schlick-GGX remapping
    float G = GeometrySmith(NdotV, NdotL, k);
    float3 F = FresnelSchlick(VdotH, F0);
    float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    // Energy split for metal/dielectric:
    // kD = (1 - F) * (1 - metallic)
    float3 kd = (1.0 - F) * (1.0 - metallic);
    // Lambert diffuse: f_d = albedo / PI
    float3 diffuse = kd * albedo / 3.14159265;
    float3 dirColor = u_dirColor.rgb * u_dirDir.w;
    Lo += (diffuse + spec) * NdotL * dirColor * vis;

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
            float3 kdP = (1.0 - Fp) * (1.0 - metallic);
            float3 diffP = kdP * albedo / 3.14159265;
            float3 pointColor = colInt.rgb * colInt.w * atten;
            Lo += (diffP + specP) * NdotLp * pointColor;
        }
    }

    // Spot lights (no shadow)
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
            float3 kdS = (1.0 - Fs) * (1.0 - metallic);
            float3 diffS = kdS * albedo / 3.14159265;
            float3 spotColor = colInt.rgb * colInt.w * atten * spot;
            Lo += (diffS + specS) * NdotLs * spotColor;
        }
    }

    float3 ambient = 0.03 * albedo * ao;
    if (u_iblParams.x > 0.5) {
        // Split-sum approximation for specular IBL:
        // L_ibl = kD * (irradiance * albedo) + prefilteredEnv(R, roughness) * (F0 * brdf.x + brdf.y)
        float3 Fibl = FresnelSchlick(saturate(dot(N, V)), F0);
        float3 kdIbl = (1.0 - Fibl) * (1.0 - metallic);
        float3 irradiance = (u_iblParams.w > 0.5)
            ? EvaluateDiffuseIrradianceFromSh(N)
            : IrradianceTex.Sample(LinearWrap, N).rgb;
        float3 diffuseIBL = irradiance * albedo;

        float3 R = reflect(-V, N);
        float3 prefiltered = PrefilterTex.SampleLevel(LinearWrap, R, roughness * u_iblParams.z).rgb;
        float2 envBrdf = BrdfLutTex.SampleLevel(LinearWrap, float2(saturate(dot(N, V)), roughness), 0).rg;
        float3 specIBL = prefiltered * (F0 * envBrdf.x + envBrdf.y);

        // AO-driven occlusion for indirect light:
        // - Diffuse IBL uses AO directly.
        // - Specular IBL uses specular-occlusion to avoid over-darkening at grazing angles.
        float diffuseOcclusion = ao;
        float specularOcclusion = SpecularOcclusion(ao, NdotV, roughness);

        ambient = (kdIbl * diffuseIBL * diffuseOcclusion +
                   specIBL * specularOcclusion) * u_iblParams.y;
    }
    float3 color = ambient + Lo;

    const int debugMode = (int)(u_debugParams.x + 0.5);
    if (debugMode == 1) { // Albedo
        return float4(saturate(albedo), 1.0);
    }
    if (debugMode == 2) { // Normal
        return float4(N * 0.5 + 0.5, 1.0);
    }
    if (debugMode == 3) { // Roughness
        return float4(roughness.xxx, 1.0);
    }
    if (debugMode == 4) { // Metallic
        return float4(metallic.xxx, 1.0);
    }
    if (debugMode == 5) { // Ambient occlusion
        return float4(ao.xxx, 1.0);
    }
    if (debugMode == 6) { // Shadow visibility
        return float4(vis.xxx, 1.0);
    }

    // Output transform:
    // Reinhard tone map: c' = c / (1 + c)
    // Gamma encode (approx sRGB): c_out = c'^(1/2.2)
    color = color / (color + 1.0);
    color = pow(color, 1.0/2.2);
    return float4(color, 1.0) * i.color;
}
