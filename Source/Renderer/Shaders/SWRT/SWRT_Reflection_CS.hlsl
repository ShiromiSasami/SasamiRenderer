//
// SWRT_Reflection_CS.hlsl
// GPU BVH ソフトウェアレイトレーシングによるリフレクションテクスチャ生成。
// 1 スレッド = 1 ピクセル で以下を処理する:
//   1. カメラレイでシーンをトレース（プライマリヒット）
//   2. ヒット点の法線から反射方向を計算
//   3. 反射レイをトレースし、ヒットした面を PBR でシェーディング
//   4. フレネル・粗さで減衰した反射色を出力テクスチャに書き込む
//
// 【NEE（Next Event Estimation）】
//   ShadePBR() 内で各ライト方向にシャドウレイを飛ばし、遮蔽がなければ
//   ライトの寄与を評価する。遮蔽されたライトは寄与をスキップする。
//
// 【マルチサンプル GGX】
//   g_samplesPerPixel > 1 のとき、Hammersley 列で GGX NDF を重点サンプリングし
//   反射方向をジッターさせることで粗いサーフェスのぼけた反射を表現する。
//

#include "SWRT/SWRT_Common.hlsli"

// --------------------------------------------------------------------------
// Per-dispatch constants
// --------------------------------------------------------------------------
cbuffer ReflectionFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;           // inverse camera view-projection

    float3 g_cameraPos;
    float  g_tMin;

    uint  g_renderWidth;
    uint  g_renderHeight;
    uint  g_updatePhaseCount;  // 1 = full frame; >1 = checkerboard phase
    uint  g_updatePhaseIndex;  // which phase to update this dispatch

    float g_maxSurfaceRoughness;    // skip reflection if roughness > this
    float g_maxPrimaryHitDistance;  // max primary ray distance
    float g_maxReflectionDistance;  // max reflection ray distance
    float g_minReflectionEnergy;    // skip if energy too low

    // Directional light
    float3 g_dirLightDir;        // direction *towards* the light
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_shadowBias;

    // Ambient
    float3 g_ambientColor;
    float  g_ambientIntensity;
    float  g_iblEnabled;
    float  g_iblIntensity;
    float  g_iblPrefilterMaxMip;
    float  g_padIbl;

    // Point light count (up to 16)
    uint  g_pointLightCount;
    uint  g_spotLightCount;
    uint  g_samplesPerPixel;  // GGX multi-sample count (1=mirror, >1=importance sampling)
    uint  g_samplingMode;     // 0=IS Only, 1=NEE Only, 2=MIS (IS+NEE)

    uint  g_frameIndex;       // monotonic frame counter for temporal Hammersley jitter
    uint  g_gbufferWidth;     // native G-Buffer resolution (full render res, >= g_renderWidth)
    uint  g_gbufferHeight;    // native G-Buffer resolution (full render res, >= g_renderHeight)
    uint  g_maxBounces;       // max reflection bounces per sample (1 = single-bounce, up to 8)
    uint  g_debugView;        // RendererEnums::GBufferDebugView
    uint  g_padDebug0;
    uint  g_padDebug1;
    uint  g_padDebug2;
};

#define SWRT_SAMPLE_IS  0u
#define SWRT_SAMPLE_NEE 1u
#define SWRT_SAMPLE_MIS 2u

#define DEBUG_VIEW_SWRT_REFLECTION_HIT_DISTANCE 14u

// ---------- G-Buffer inputs ----------
// GBufferNormal  (R16G16B16A16_FLOAT): xyz = N*0.5+0.5 (encoded normal), w = camera distance
// GBufferMaterial(R8G8B8A8_UNORM):     r = roughness, g = metallic
// GBufferAlbedo  (R8G8B8A8_UNORM):     rgb = base color
Texture2D<float4> g_gbufferNormal   : register(t6);
Texture2D<float4> g_gbufferMaterial : register(t7);
Texture2D<float4> g_gbufferAlbedo   : register(t8);
TextureCube       g_iblPrefilter    : register(t9);

SamplerState LinearClamp : register(s0);

RWTexture2D<float4> g_reflOutput : register(u0);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

float3 ComputeCameraRayDir(uint2 pixel, uint2 resolution)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(resolution.x)) * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(resolution.y)) * 2.0f + 1.0f;
    float4 dir = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    return normalize(dir.xyz / dir.w - g_cameraPos);
}

// --------------------------------------------------------------------------
// Hammersley low-discrepancy sequence (deterministic, no temporal noise)
// --------------------------------------------------------------------------
float2 Hammersley(uint i, uint N)
{
    uint bits = (i << 16u) | (i >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(N), float(bits) * 2.3283064365386963e-10f);
}

// GGX NDF importance sampling: returns half-vector in tangent space
float3 SampleGGX_H(float2 Xi, float roughness)
{
    float a   = roughness * roughness;
    float phi = 6.28318530718f * Xi.x;
    float cosT = sqrt((1.0f - Xi.y) / max(1.0f + (a*a - 1.0f) * Xi.y, 1e-7f));
    float sinT = sqrt(max(0.0f, 1.0f - cosT * cosT));
    return float3(sinT * cos(phi), sinT * sin(phi), cosT);
}

// Transform tangent-space vector to world space aligned with N
float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
}

// --------------------------------------------------------------------------
// Schlick fresnel
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    float f = pow(saturate(1.0f - cosTheta), 5.0f);
    return F0 + (1.0f - F0) * f;
}

// GGX NDF
float GGX_D(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / (3.14159265f * d * d);
}

// Smith GGX visibility (approximation)
float GGX_V(float NdotL, float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gL = NdotL / (NdotL * (1.0f - k) + k);
    float gV = NdotV / (NdotV * (1.0f - k) + k);
    return gL * gV;
}

// Evaluate PBR (directional light only for reflection shading)
float3 ShadeDirectPBR(float3 pos, float3 N, float3 V, GpuMaterial mat)
{
    float roughness = saturate(mat.roughness);
    float3 F0 = SWRT_MaterialF0(mat);
    float3 diffuseReflectance = SWRT_MaterialDiffuseReflectance(mat);

    float3 outColor = max(mat.emissive, 0.0f);

    // ---- 直接光 (NEE / MIS) ----------------------------------------
    // IS Only モードのときはスキップ。NEE Only / MIS では全ライトへ shadow ray。
    if (g_samplingMode != SWRT_SAMPLE_IS)
    {
        // Directional light
        {
            float3 L     = normalize(g_dirLightDir);
            float  NdotL = max(dot(N, L), 0.0f);
            if (NdotL > 0.0f)
            {
                bool inShadow = TraceAnyHit(OffsetRay(pos, N), L, g_tMin, 200.0f);
                if (!inShadow)
                {
                    float3 H     = normalize(L + V);
                    float  NdotV = max(dot(N, V), 0.001f);
                    float  NdotH = saturate(dot(N, H));
                    float  VdotH = saturate(dot(V, H));

                    float3 F   = FresnelSchlick(VdotH, F0);
                    float  D   = GGX_D(NdotH, max(roughness, 0.05f));
                    float  Vis = GGX_V(NdotL, NdotV, max(roughness, 0.05f));

                    float3 specular = (F * D * Vis) / max(4.0f * NdotL * NdotV, 0.001f);
                    float3 diffuse  = diffuseReflectance / 3.14159265f;
                    float3 kd = (1.0f - F);

                    outColor += (kd * diffuse + specular) * NdotL * g_dirLightColor * g_dirLightIntensity;
                }
            }
        }

        // Note: point/spot light NEE removed — G-Buffer path uses directional light only.
        // The reflection hit point's local lights are evaluated via ShadePBR at the
        // secondary hit, which can be extended in future iterations.
    } // end NEE block

    // ---- Ambient/Sky (IS Only / MIS) ----------------------------------------
    // NEE Only モードのときはスキップ。
    if (g_samplingMode != SWRT_SAMPLE_NEE)
    {
        float3 diffuseIblRadiance = g_ambientColor * g_ambientIntensity;
        float3 specularIblRadiance = diffuseIblRadiance;
        if (g_iblEnabled > 0.5f)
        {
            diffuseIblRadiance += g_iblPrefilter.SampleLevel(
                LinearClamp, N, max(g_iblPrefilterMaxMip, 0.0f)).rgb * g_iblIntensity;

            float3 R = reflect(-V, N);
            float  specularMip = saturate(roughness) * max(g_iblPrefilterMaxMip, 0.0f);
            specularIblRadiance = g_iblPrefilter.SampleLevel(
                LinearClamp, R, specularMip).rgb * g_iblIntensity;
        }

        float  NdotV = saturate(dot(N, V));
        float3 F_ibl = FresnelSchlick(NdotV, F0);
        float  smoothness = 1.0f - roughness;
        float  dielectricVisibility = 0.08f * smoothness * smoothness;
        float3 specularVisibility = max(F_ibl, float3(dielectricVisibility,
                                                       dielectricVisibility,
                                                       dielectricVisibility));

        outColor += diffuseReflectance * diffuseIblRadiance;
        outColor += specularVisibility * specularIblRadiance;
    }

    return outColor;
}

float3 EvaluateReflectionHitAmbientFloor(GpuMaterial mat)
{
    // Small floor to avoid fully black traced hits indoors without injecting
    // sky color. Use the visible material color rather than diffuse-only color
    // so metallic reflection hits do not collapse to black.
    static const float kHitAmbientFloor = 0.06f;
    float3 diffuseColor = SWRT_MaterialDiffuseReflectance(mat);
    float3 visibleColor = lerp(diffuseColor, mat.baseColor.rgb, saturate(mat.metallic));
    return visibleColor * kHitAmbientFloor;
}

float3 SampleReflectionEnvironment(float3 dir, float roughness)
{
    float3 skyColor = g_ambientColor * g_ambientIntensity;
    if (g_iblEnabled > 0.5f)
    {
        const float mip = saturate(roughness) * max(g_iblPrefilterMaxMip, 0.0f);
        skyColor = g_iblPrefilter.SampleLevel(LinearClamp, dir, mip).rgb * g_iblIntensity;
    }
    return skyColor;
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
[numthreads(16, 16, 1)]
void CS_Reflection(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight)
        return;

    // Phase update: skip pixels that don't belong to this phase
    if (g_updatePhaseCount > 1u)
    {
        uint pixelPhase = (id.x + id.y * g_renderWidth) % g_updatePhaseCount;
        if (pixelPhase != g_updatePhaseIndex)
            return;
    }

    // ---- Read primary surface from G-Buffer (no primary ray needed) ----
    // GBufferNormal.w stores camera distance written by CookTorranceGGX_PS.hlsl.
    // depth == 0 means sky / no geometry was rasterised for this pixel.
    //
    // The G-Buffer is at native render resolution (g_gbufferWidth × g_gbufferHeight),
    // but this CS may dispatch at a reduced reflection resolution (g_renderWidth × g_renderHeight).
    // Scale id.xy from reflection-space to G-Buffer-space so each reflection pixel reads
    // the correct texel instead of a biased top-left region of the screen.
    // Use float arithmetic to avoid integer truncation when gbuffer resolution
    // is not an exact integer multiple of the reflection render resolution.
    uint2 gbufPx = uint2(
        min(uint(float(id.x) * float(g_gbufferWidth)  / float(g_renderWidth)),  g_gbufferWidth  - 1u),
        min(uint(float(id.y) * float(g_gbufferHeight) / float(g_renderHeight)), g_gbufferHeight - 1u));
    float4 gbufNormal   = g_gbufferNormal.Load(int3(gbufPx, 0));
    float  linearDepth  = gbufNormal.w;

    if (linearDepth <= 0.0f)
    {
        if (g_debugView == DEBUG_VIEW_SWRT_REFLECTION_HIT_DISTANCE)
        {
            g_reflOutput[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
            return;
        }
        g_reflOutput[id.xy] = float4(0, 0, 0, 0); // sky pixel
        return;
    }

    // Check TLAS is valid (needed for reflection ray)
    if (g_tlasNodes[0].leftChild == 0 && g_tlasNodes[0].rightOrCount == 0)
    {
        if (g_debugView == DEBUG_VIEW_SWRT_REFLECTION_HIT_DISTANCE)
        {
            g_reflOutput[id.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
            return;
        }
        g_reflOutput[id.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Decode world-space normal (stored as N*0.5+0.5)
    float3 hitNorm = normalize(gbufNormal.xyz * 2.0f - 1.0f);

    // Reconstruct world position from linearDepth (camera-to-surface distance) + camera ray.
    // This avoids the NDC→world inversion precision error that caused reflection offset.
    // GBuffer.w now stores length(worldPos - cameraPos) written by CookTorranceGGX_PS.hlsl.
    float ndcX =  ((float(gbufPx.x) + 0.5f) / float(g_gbufferWidth))  * 2.0f - 1.0f;
    float ndcY = -((float(gbufPx.y) + 0.5f) / float(g_gbufferHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 rayDir   = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    float3 hitPos   = g_cameraPos + rayDir * linearDepth;

    // Camera view direction
    float3 V = normalize(-rayDir);

    // Ensure normal faces camera
    if (dot(hitNorm, V) < 0.0f)
        hitNorm = -hitNorm;

    // Read material from G-Buffer (same gbufPx scaling as above)
    float4 gbufMaterial = g_gbufferMaterial.Load(int3(gbufPx, 0));
    float4 gbufAlbedo   = g_gbufferAlbedo.Load(int3(gbufPx, 0));
    float  roughness    = gbufMaterial.r;
    float  metallic     = gbufMaterial.g;
    float3 albedo       = gbufAlbedo.rgb;

    if (g_debugView == DEBUG_VIEW_SWRT_REFLECTION_HIT_DISTANCE)
    {
        float3 reflDir = reflect(rayDir, hitNorm);
        HitResult hit = TraceClosestHit(
            OffsetRay(hitPos, hitNorm), reflDir, g_tMin, g_maxReflectionDistance);

        // Miss is the far end of the ramp. Use a practical near-field range and
        // soften the ramp so small hit distances do not collapse into solid black.
        float debugRange = min(max(g_maxReflectionDistance, 1e-4f), 5.0f);
        float distance01 = hit.hit
            ? pow(saturate(max(hit.t, 0.0f) / debugRange), 0.45f)
            : 1.0f;
        g_reflOutput[id.xy] = float4(distance01.xxx, 1.0f);
        return;
    }

    // Roughness-based fade: smooth falloff instead of hard cutoff, but keep a
    // small metallic tail so rough metal still contributes blurred reflection.
    float roughnessFade = 1.0f - smoothstep(g_maxSurfaceRoughness * 0.7f,
                                            g_maxSurfaceRoughness,
                                            roughness);
    float roughnessTail = saturate((roughness - g_maxSurfaceRoughness) /
                                   max(1.0f - g_maxSurfaceRoughness, 1e-4f));
    roughnessFade = max(roughnessFade, roughnessTail * lerp(0.08f, 0.22f, metallic));
    // Energy estimate for early-out (metallic surfaces have more energy)
    float energyEstimate = roughnessFade * lerp(0.04f, 1.0f, metallic);
    if (energyEstimate < g_minReflectionEnergy)
    {
        g_reflOutput[id.xy] = float4(0, 0, 0, 0);
        return;
    }

    // --------------------------------------------------------------------------
    // Multi-sample, multi-bounce GGX reflection loop
    // --------------------------------------------------------------------------
    // Outer loop: g_samplesPerPixel GGX-sampled directions (importance sampling).
    // Inner loop: up to g_maxBounces bounces per sample ray.
    //
    // Throughput convention:
    //   bounce 0 → throughput = 1.0  (Fresnel applied by PBR_PS via alpha)
    //   bounce 1 → throughput *= F0 * roughFade0
    //   bounce N → throughput *= F(N-1) * roughFade(N-1)
    // --------------------------------------------------------------------------
    float3 reflColorAccum = (float3)0;
    float  weightAccum    = 0.0f;

    uint sampleCount = max(1u, g_samplesPerPixel);
    uint maxBounces  = max(1u, g_maxBounces);

    for (uint s = 0; s < sampleCount; ++s)
    {
        // --- State for this sample path ---
        float3 pathOrigin = hitPos;
        float3 pathNorm   = hitNorm;
        float3 pathInDir  = rayDir;        // incoming direction (towards surface)
        float  pathRough  = roughness;     // roughness of current surface
        float3 throughput = float3(1.0f, 1.0f, 1.0f);
        float3 sampleColor = float3(0.0f, 0.0f, 0.0f);

        for (uint b = 0; b < maxBounces; ++b)
        {
            // ---- Sample reflection direction ----
            float3 reflDir;
            bool mirror = (pathRough < 0.01f);
            if (mirror)
            {
                reflDir = reflect(pathInDir, pathNorm);
            }
            else
            {
                // Per-sample, per-bounce Hammersley index with per-pixel spatial jitter.
                // Using a pixel-derived spatial hash instead of a frame-index temporal jitter
                // keeps the sample pattern stable across frames (no per-frame flickering
                // when no temporal accumulation is applied to the reflection texture).
                // Successive bounces and samples are decorrelated by their index offset.
                uint   seqIndex = s * maxBounces + b;
                uint   seqTotal = sampleCount * maxBounces;
                float2 Xi = Hammersley(seqIndex, seqTotal);

                // Cranley-Patterson rotation: hash pixel + frame index to get a
                // per-pixel, per-frame shift in [0,1)^2 that decorrelates both
                // spatial neighbours and successive frames.  Including g_frameIndex
                // gives temporal variation so the noise pattern changes each frame,
                // which lets a downstream TAA / temporal accumulation pass resolve it.
                uint ph = id.x * 1973u ^ id.y * 9277u ^ seqIndex * 26699u ^ g_frameIndex * 6271u;
                ph = (ph ^ (ph >> 13u)) * 1540483477u;
                ph =  ph ^ (ph >> 15u);
                float2 pixJitter = float2(float( ph        & 0xFFFFu) / 65536.0f,
                                         float((ph >> 16u) & 0xFFFFu) / 65536.0f);
                Xi = frac(Xi + pixJitter);

                float3 H  = TangentToWorld(SampleGGX_H(Xi, pathRough), pathNorm);
                reflDir   = reflect(pathInDir, H);
                if (dot(reflDir, pathNorm) <= 0.0f)
                    break; // invalid sample: bail out of bounce loop
            }

            // ---- Trace this bounce ----
            // Use NVIDIA's error-bounded OffsetRay to spawn the ray just above the
            // surface without self-intersection.  This is superior to a fixed or
            // distance-adaptive bias because it derives the minimum safe offset from
            // the ULP of the hit position, correctly scaling with world-space magnitude.
            HitResult hit = TraceClosestHit(
                OffsetRay(pathOrigin, pathNorm), reflDir, g_tMin, g_maxReflectionDistance);

            if (!hit.hit)
            {
                // Missed geometry → sky contribution
                sampleColor += throughput * SampleReflectionEnvironment(reflDir, pathRough);
                break;
            }

            // ---- Shade hit point (NEE) ----
            GpuInstanceInfo inst    = g_instances[hit.instanceIndex];
            GpuMaterial     mat     = g_materials[inst.materialIndex];
            float3 hitPos_b  = pathOrigin + reflDir * hit.t;
            float3 hitNorm_b = GetWorldNormal(hit);
            if (dot(hitNorm_b, -reflDir) < 0.0f)
                hitNorm_b = -hitNorm_b;

            float3 V_b = normalize(-reflDir);
            float3 hitDirect = ShadeDirectPBR(hitPos_b, hitNorm_b, V_b, mat);
            float3 hitFloor  = EvaluateReflectionHitAmbientFloor(mat);
            sampleColor += throughput * (hitDirect + hitFloor);

            // ---- Check whether to continue bouncing ----
            bool lastBounce = (b + 1u >= maxBounces);
            if (lastBounce)
                break;

            // Surfaces rougher than the threshold absorb rather than reflect.
            float hitRoughFade = 1.0f - smoothstep(g_maxSurfaceRoughness * 0.7f,
                                                    g_maxSurfaceRoughness, mat.roughness);
            if (hitRoughFade <= 0.0f)
                break;

            // Fresnel attenuation for this bounce
            float3 F0_b  = SWRT_MaterialF0(mat);
            float3 F_b   = FresnelSchlick(max(dot(hitNorm_b, V_b), 0.0f), F0_b);
            throughput  *= F_b * hitRoughFade;

            // Russian roulette energy cutoff (avoid tracing near-zero paths)
            float energy = dot(throughput, float3(0.2126f, 0.7152f, 0.0722f));
            if (energy < 0.02f)
                break;

            // Advance path
            pathOrigin = hitPos_b;
            pathNorm   = hitNorm_b;
            pathInDir  = reflDir;
            pathRough  = mat.roughness;
        }

        reflColorAccum += sampleColor;
        weightAccum    += 1.0f;
    }

    float3 reflColor = (weightAccum > 0.0f) ? (reflColorAccum / weightAccum) : (float3)0;

    // Roughness attenuation only — Fresnel is applied per-channel in PBR_PS.hlsl
    // using the surface's F0 so that colored metals get correct per-channel tint.
    // Storing scalar fresnelWeight here (the old approach) collapsed the float3 Fresnel
    // to a luminance value, which caused green/blue bleed on red/gold metals.
    float roughnessAtten = roughnessFade * roughnessFade;

    // Store raw reflColor in RGB, roughnessAtten in alpha.
    // PBR_PS.hlsl multiplies by per-channel F (FresnelSchlick) at compositing time.
    g_reflOutput[id.xy] = float4(reflColor, roughnessAtten);
}
