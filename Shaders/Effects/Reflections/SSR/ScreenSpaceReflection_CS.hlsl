cbuffer SSRConstants : register(b0)
{
    row_major float4x4 u_cameraPV;
    row_major float4x4 u_cameraInvPV;
    float4 u_screenSize;    // xy: size, zw: inverse size
    float4 u_traceParams0;  // x: max distance, y: thickness, z: max steps, w: max roughness
    float4 u_traceParams1;  // x: refinement steps, y: edge fade width, z: normal offset, w: intensity
    float4 u_cameraPos;     // xyz: world-space camera position
    float4 u_reserved4;     // unused (PushCameraCB extra4 slot)
    float4 u_iblParams;     // x: ibl enabled, y: ibl intensity, z: ibl prefilter max mip, w: unused
}

Texture2D<float>  DepthTex      : register(t0);
Texture2D<float4> NormalTex     : register(t1);
Texture2D<float4> MaterialTex   : register(t2);
Texture2D<float4> SceneColorTex : register(t3);
TextureCube<float4> IblPrefilterTex : register(t5);
RWTexture2D<float4> ReflectionOut : register(u0);
SamplerState LinearClamp : register(s0);

#include "Shared/Common/WorldPosReconstruction.hlsli"

float2 ProjectToUv(float3 worldPos, out float ndcDepth, out bool valid)
{
    float4 clip = mul(float4(worldPos, 1.0f), u_cameraPV);
    valid = clip.w > 1e-5f;
    float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    ndcDepth = ndc.z;
    valid = valid && ndcDepth > 0.0f && ndcDepth < 1.0f &&
            all(ndc.xy >= -1.0f) && all(ndc.xy <= 1.0f);
    return ndc.xy * float2(0.5f, -0.5f) + 0.5f;
}

float EdgeFade(float2 uv)
{
    float edge = min(min(uv.x, uv.y), min(1.0f - uv.x, 1.0f - uv.y));
    return saturate(edge / max(u_traceParams1.y, 1e-4f));
}

bool IsValidDepth(float depth)
{
    return depth > 1e-5f && depth < 0.99999f;
}

float3 DecodeNormal(float4 encodedNormal)
{
    float3 n = encodedNormal.xyz * 2.0f - 1.0f;
    float lenSq = dot(n, n);
    return (lenSq > 1e-5f) ? (n * rsqrt(lenSq)) : float3(0.0f, 1.0f, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint width = (uint)u_screenSize.x;
    uint height = (uint)u_screenSize.y;
    if (pixel.x >= width || pixel.y >= height) {
        return;
    }

    float2 uv = (float2(pixel) + 0.5f) * u_screenSize.zw;
    float depth = DepthTex.Load(int3(pixel, 0));
    float4 material = MaterialTex.Load(int3(pixel, 0));
    float roughness = saturate(material.r);
    float reflectionMask = saturate(material.a);
    if (!IsValidDepth(depth) || reflectionMask <= 0.001f || roughness >= u_traceParams0.w) {
        ReflectionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 normalSample = NormalTex.Load(int3(pixel, 0));
    float3 normal = DecodeNormal(normalSample);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 viewDir = normalize(u_cameraPos.xyz - worldPos);
    if (dot(normal, viewDir) <= 1e-4f) {
        ReflectionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 rayDir = normalize(reflect(-viewDir, normal));
    if (dot(rayDir, viewDir) >= -1e-4f) {
        ReflectionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float maxDistance = max(u_traceParams0.x, 0.01f);
    float thickness = max(u_traceParams0.y, 1e-4f);
    uint stepCount = max(1u, (uint)round(u_traceParams0.z));
    uint refineSteps = max(0u, (uint)round(u_traceParams1.x));
    float3 rayOrigin = worldPos + normal * u_traceParams1.z;

    bool hit = false;
    float2 hitUv = 0.0f;
    float hitConfidence = 0.0f;
    float prevT = 0.0f;
    float currT = 0.0f;

    [loop]
    for (uint stepIndex = 1u; stepIndex <= stepCount; ++stepIndex) {
        float normalizedStep = (float)stepIndex / (float)stepCount;
        currT = maxDistance * normalizedStep * normalizedStep;
        float3 samplePos = rayOrigin + rayDir * currT;
        float sampleNdcDepth = 0.0f;
        bool validProjection = false;
        float2 sampleUv = ProjectToUv(samplePos, sampleNdcDepth, validProjection);
        if (!validProjection) {
            break;
        }

        uint2 samplePixel = uint2(min(floor(sampleUv * u_screenSize.xy), u_screenSize.xy - 1.0f));
        float sceneDepth = DepthTex.Load(int3(samplePixel, 0));
        if (!IsValidDepth(sceneDepth)) {
            prevT = currT;
            continue;
        }

        float3 sceneWorldPos = ReconstructWorldPos(sampleUv, sceneDepth);
        float sceneT = dot(sceneWorldPos - rayOrigin, rayDir);
        float rayDelta = currT - sceneT;
        float adaptiveThickness = thickness * (1.0f + normalizedStep * 2.0f);
        if (sceneT > 0.0f && rayDelta >= -adaptiveThickness * 0.25f && rayDelta <= adaptiveThickness) {
            float lowT = prevT;
            float highT = currT;
            hitUv = sampleUv;
            [loop]
            for (uint refine = 0u; refine < refineSteps; ++refine) {
                float midT = 0.5f * (lowT + highT);
                float3 midPos = rayOrigin + rayDir * midT;
                float midNdcDepth = 0.0f;
                bool midValid = false;
                float2 midUv = ProjectToUv(midPos, midNdcDepth, midValid);
                if (!midValid) {
                    highT = midT;
                    continue;
                }
                uint2 midPixel = uint2(min(floor(midUv * u_screenSize.xy), u_screenSize.xy - 1.0f));
                float midSceneDepth = DepthTex.Load(int3(midPixel, 0));
                if (!IsValidDepth(midSceneDepth)) {
                    lowT = midT;
                    continue;
                }
                float3 midSceneWorldPos = ReconstructWorldPos(midUv, midSceneDepth);
                float midSceneT = dot(midSceneWorldPos - rayOrigin, rayDir);
                if (midT >= midSceneT) {
                    highT = midT;
                    hitUv = midUv;
                } else {
                    lowT = midT;
                }
            }
            hit = true;
            hitConfidence = EdgeFade(hitUv) * (1.0f - smoothstep(0.45f, u_traceParams0.w, roughness));
            break;
        }
        prevT = currT;
    }

    if (!hit || hitConfidence <= 0.0f) {
        // Ray left the screen (or found no valid hit within the step budget) before
        // resolving against depth: fall back to the prefiltered IBL cubemap sampled
        // along the reflection ray, same roughness-to-mip mapping as SWRT reflections
        // (SampleReflectionEnvironment in SWRT_Reflection_Shading.hlsli).
        if (u_iblParams.x > 0.5f) {
            float mip = saturate(roughness) * u_iblParams.z;
            float3 iblColor = IblPrefilterTex.SampleLevel(LinearClamp, rayDir, mip).rgb * u_iblParams.y;
            float iblConfidence = saturate(reflectionMask * u_traceParams1.w);
            ReflectionOut[pixel] = float4(iblColor, iblConfidence);
        } else {
            ReflectionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return;
    }

    float3 radiance = SceneColorTex.SampleLevel(LinearClamp, hitUv, 0).rgb;
    float confidence = saturate(hitConfidence * reflectionMask * u_traceParams1.w);
    ReflectionOut[pixel] = float4(radiance, confidence);
}
