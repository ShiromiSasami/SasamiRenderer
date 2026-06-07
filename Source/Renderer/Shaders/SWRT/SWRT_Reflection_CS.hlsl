//
// SWRT_Reflection_CS.hlsl
// GPU BVH 繧ｽ繝輔ヨ繧ｦ繧ｧ繧｢繝ｬ繧､繝医Ξ繝ｼ繧ｷ繝ｳ繧ｰ縺ｫ繧医ｋ繝ｪ繝輔Ξ繧ｯ繧ｷ繝ｧ繝ｳ繝・け繧ｹ繝√Ε逕滓・縲・
// 1 繧ｹ繝ｬ繝・ラ = 1 繝斐け繧ｻ繝ｫ 縺ｧ莉･荳九ｒ蜃ｦ逅・☆繧・
//   1. 繧ｫ繝｡繝ｩ繝ｬ繧､縺ｧ繧ｷ繝ｼ繝ｳ繧偵ヨ繝ｬ繝ｼ繧ｹ・医・繝ｩ繧､繝槭Μ繝偵ャ繝茨ｼ・
//   2. 繝偵ャ繝育せ縺ｮ豕慕ｷ壹°繧牙渚蟆・婿蜷代ｒ險育ｮ・
//   3. 蜿榊ｰ・Ξ繧､繧偵ヨ繝ｬ繝ｼ繧ｹ縺励√ヲ繝・ヨ縺励◆髱｢繧・PBR 縺ｧ繧ｷ繧ｧ繝ｼ繝・ぅ繝ｳ繧ｰ
//   4. 繝輔Ξ繝阪Ν繝ｻ邊励＆縺ｧ貂幄｡ｰ縺励◆蜿榊ｰ・牡繧貞・蜉帙ユ繧ｯ繧ｹ繝√Ε縺ｫ譖ｸ縺崎ｾｼ繧
//
// 縲侵EE・・ext Event Estimation・峨・
//   ShadePBR() 蜀・〒蜷・Λ繧､繝域婿蜷代↓繧ｷ繝｣繝峨え繝ｬ繧､繧帝｣帙・縺励・・阡ｽ縺後↑縺代ｌ縺ｰ
//   繝ｩ繧､繝医・蟇・ｸ弱ｒ隧穂ｾ｡縺吶ｋ縲る・阡ｽ縺輔ｌ縺溘Λ繧､繝医・蟇・ｸ弱ｒ繧ｹ繧ｭ繝・・縺吶ｋ縲・
//
// 縲舌・繝ｫ繝√し繝ｳ繝励Ν GGX縲・
//   g_samplesPerPixel > 1 縺ｮ縺ｨ縺阪？ammersley 蛻励〒 GGX NDF 繧帝㍾轤ｹ繧ｵ繝ｳ繝励Μ繝ｳ繧ｰ縺・
//   蜿榊ｰ・婿蜷代ｒ繧ｸ繝・ち繝ｼ縺輔○繧九％縺ｨ縺ｧ邊励＞繧ｵ繝ｼ繝輔ぉ繧ｹ縺ｮ縺ｼ縺代◆蜿榊ｰ・ｒ陦ｨ迴ｾ縺吶ｋ縲・
//

#include "SWRT/SWRT_Common.hlsli"
#include "SWRT/SWRT_LightTypes.hlsli"
#include "ProceduralSky/ProceduralSky.hlsli"

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
    float  g_proceduralSkyEnabled;  // 1.0 = evaluate ComputeSkyColor on ray miss (no IBL prefilter)

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

SamplerState LinearClamp : register(s0);

TextureCube<float4> g_iblPrefilterTex : register(t9);

RWTexture2D<float4> g_reflOutput : register(u0);

StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

#include "SWRT_Reflection_Shading.hlsli"
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
    // The G-Buffer is at native render resolution (g_gbufferWidth ﾃ・g_gbufferHeight),
    // but this CS may dispatch at a reduced reflection resolution (g_renderWidth ﾃ・g_renderHeight).
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
    // This avoids the NDC竊蜘orld inversion precision error that caused reflection offset.
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
    float  reflectionStrength = saturate(gbufMaterial.a);
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

    if (reflectionStrength <= 0.001f)
    {
        g_reflOutput[id.xy] = float4(0, 0, 0, 0);
        return;
    }

    // Roughness-based fade: smooth falloff instead of a hard cutoff. Whether the
    // primary surface is reflective is authored through GBufferMaterial.a.
    float roughnessFade = 1.0f - smoothstep(g_maxSurfaceRoughness * 0.7f,
                                            g_maxSurfaceRoughness,
                                            roughness);
    float energyEstimate = roughnessFade * reflectionStrength;
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
    //   bounce 0 竊・throughput = 1.0  (Fresnel applied by PBR_PS via alpha)
    //   bounce 1 竊・throughput *= F0 * roughFade0
    //   bounce N 竊・throughput *= F(N-1) * roughFade(N-1)
    // --------------------------------------------------------------------------
    float3 reflColorAccum = (float3)0;
    float  weightAccum    = 0.0f;
    float  resolvedAccum  = 0.0f;

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
        bool sampleResolvedRadiance = false;

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
                sampleColor += throughput * SampleReflectionEnvironment(reflDir, pathRough);
                sampleResolvedRadiance = true;
                break;
            }
            sampleResolvedRadiance = true;

            // ---- Shade hit point (NEE) ----
            GpuInstanceInfo inst    = g_instances[hit.instanceIndex];
            GpuMaterial     mat     = g_materials[inst.materialIndex];
            float3 hitPos_b  = pathOrigin + reflDir * hit.t;
            float3 hitNorm_b = GetWorldNormal(hit);
            if (dot(hitNorm_b, -reflDir) < 0.0f)
                hitNorm_b = -hitNorm_b;

            float3 V_b = normalize(-reflDir);
            if (SWRT_IsTransparentMaterial(mat))
            {
                bool transparentResolved = false;
                sampleColor += ResolveReflectionRayThroughTransparent(
                    pathOrigin,
                    reflDir,
                    g_maxReflectionDistance,
                    throughput,
                    transparentResolved);
                sampleResolvedRadiance = sampleResolvedRadiance || transparentResolved;
                break;
            }

            float3 hitDirect = ShadeDirectPBR(hitPos_b, hitNorm_b, V_b, mat);
            sampleColor += throughput * hitDirect;

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
        resolvedAccum  += sampleResolvedRadiance ? 1.0f : 0.0f;
    }

    float3 reflColor = (weightAccum > 0.0f) ? (reflColorAccum / weightAccum) : (float3)0;

    // Roughness attenuation only 窶・Fresnel is applied per-channel in PBR_PS.hlsl
    // using the surface's F0 so that colored metals get correct per-channel tint.
    // Storing scalar fresnelWeight here (the old approach) collapsed the float3 Fresnel
    // to a luminance value, which caused green/blue bleed on red/gold metals.
    float reflectionConfidence = (weightAccum > 0.0f) ? saturate(resolvedAccum / weightAccum) : 0.0f;

    // Store scene-hit reflection radiance in RGB and hit confidence in alpha.
    // Misses stay black; the composite pass suppresses them via alpha.
    g_reflOutput[id.xy] = float4(reflColor, reflectionConfidence);
}
