Texture2D AlbedoTex    : register(t0);
Texture2D ShadowMapTex : register(t1);
SamplerState LinearWrap : register(s0);

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

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP;
    float4 u_dirDir;     // xyz: forward dir, w: intensity
    float4 u_dirColor;   // rgb: color
    float4 u_lightCounts; // x: pointCount, y: spotCount
}

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

// --- PBR helpers (minimal GGX implementation with constants) ---
float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float NdotV, float NdotL, float k)
{
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
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
    return acc / 9.0;
}

float4 PSMain(PSInput i) : SV_TARGET
{
    // Albedo in sRGB assumed; if not sRGB-corrected by sampler state, apply manual gamma.
    float3 albedo = AlbedoTex.Sample(LinearWrap, i.uv).rgb;

    // Minimal PBR params (constants for now)
    float metallic = 0.0;
    float roughness = 0.5; // perceptual roughness [0..1]
    float3 N = normalize(i.worldN);
    float3 V = normalize(float3(0,0,1)); // approx camera view in view-space; replace with world V if available

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 Lo = 0.0;
    float vis = ShadowVisibility(i.lightPos);

    // Directional light (shadowed)
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
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo / 3.14159265;
    float3 dirColor = u_dirColor.rgb * u_dirDir.w;
    Lo += (diffuse + spec) * NdotL * dirColor * vis;

    // Point lights (no shadow)
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

    // Simple ambient (placeholder for IBL irradiance)
    float3 ambient = 0.03 * albedo;
    float3 color = ambient + Lo;

    // Tone map (simple curve) and gamma out
    color = color / (color + 1.0);
    color = pow(color, 1.0/2.2);
    return float4(color, 1.0) * i.color;
}
