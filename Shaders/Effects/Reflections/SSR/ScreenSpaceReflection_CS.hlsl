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

// Linear view depth (clip-space w) of a depth-buffer sample. For ndc = clip / w,
// mul(ndc, invPV).w == 1 / clip.w, so no world-space reconstruction is needed.
float SceneViewDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
    float invW = mul(ndc, u_cameraInvPV).w;
    return 1.0f / max(invW, 1e-9f);
}

// Interleaved gradient noise (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare").
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// Undoes the xy permutation applied for the DDA major axis.
float2 UnpermutePixelPos(float2 p, bool permuted)
{
    float2 result = p;
    if (permuted) {
        result = p.yx;
    }
    return result;
}

float EdgeFade(float2 uv)
{
    float edge = min(min(uv.x, uv.y), min(1.0f - uv.x, 1.0f - uv.y));
    return saturate(edge / max(u_traceParams1.y, 1e-4f));
}

bool IsValidDepth(float depth)
{
    // Reverse-Z: near = 1.0, far = 0.0. depth <= 1e-5f rejects the cleared far/background
    // plane and depth >= 0.99999f rejects the degenerate near-plane clip; the numeric
    // bounds are unchanged from the forward-Z form since both extremes are still excluded.
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

    // Screen-space DDA march with a view-depth thickness test, after McGuire & Mara 2014,
    // "Efficient GPU Screen-Space Ray Tracing", JCGT 3(4). 1 / w is affine in screen space,
    // so it is interpolated linearly along the projected line (perspective-correct depth).
    const float NEAR_W_EPSILON = 1e-3f;
    float4 clip0 = mul(float4(rayOrigin, 1.0f), u_cameraPV);
    float4 clip1 = mul(float4(rayOrigin + rayDir * maxDistance, 1.0f), u_cameraPV);
    if (clip0.w > NEAR_W_EPSILON) {
        if (clip1.w < NEAR_W_EPSILON) {
            // Keep the segment in front of the camera.
            float nearT = (clip0.w - NEAR_W_EPSILON) / (clip0.w - clip1.w);
            clip1 = lerp(clip0, clip1, nearT);
        }

        float k0 = 1.0f / clip0.w;
        float k1 = 1.0f / clip1.w;
        float2 p0 = (clip0.xy * k0 * float2(0.5f, -0.5f) + 0.5f) * u_screenSize.xy;
        float2 p1 = (clip1.xy * k1 * float2(0.5f, -0.5f) + 0.5f) * u_screenSize.xy;
        float2 lineDelta = p1 - p0;
        float dk = k1 - k0;

        // Clip the screen-space segment to the viewport rectangle.
        float clipFraction = 1.0f;
        if (lineDelta.x > 1e-6f) {
            clipFraction = min(clipFraction, (u_screenSize.x - p0.x) / lineDelta.x);
        } else if (lineDelta.x < -1e-6f) {
            clipFraction = min(clipFraction, -p0.x / lineDelta.x);
        }
        if (lineDelta.y > 1e-6f) {
            clipFraction = min(clipFraction, (u_screenSize.y - p0.y) / lineDelta.y);
        } else if (lineDelta.y < -1e-6f) {
            clipFraction = min(clipFraction, -p0.y / lineDelta.y);
        }
        clipFraction = max(clipFraction, 0.0f);
        lineDelta *= clipFraction;
        dk *= clipFraction;

        // DDA along the major axis: permute xy so x is always the long axis.
        bool permuted = abs(lineDelta.y) > abs(lineDelta.x);
        if (permuted) {
            p0 = p0.yx;
            lineDelta = lineDelta.yx;
        }

        float totalPixels = abs(lineDelta.x);
        if (totalPixels >= 1.0f) {
            float pixelStep = max(1.0f, totalPixels / (float)stepCount);
            float totalSteps = totalPixels / pixelStep;
            uint iterations = min(stepCount, (uint)ceil(totalSteps));
            float jitter = InterleavedGradientNoise(float2(pixel));

            float prevParam = 0.0f;
            [loop]
            for (uint stepIndex = 1u; stepIndex <= iterations; ++stepIndex) {
                float currParam = min((float)stepIndex + jitter, totalSteps) / totalSteps;
                float2 samplePos = UnpermutePixelPos(p0 + lineDelta * currParam, permuted);
                uint2 samplePixel = uint2(clamp(floor(samplePos), float2(0.0f, 0.0f), u_screenSize.xy - 1.0f));
                float2 sampleUv = (float2(samplePixel) + 0.5f) * u_screenSize.zw;
                float sceneDepth = DepthTex.Load(int3(samplePixel, 0));
                if (!IsValidDepth(sceneDepth)) {
                    prevParam = currParam;
                    continue;
                }

                float sceneW = SceneViewDepth(sampleUv, sceneDepth);
                float prevRayW = 1.0f / (k0 + dk * prevParam);
                float currRayW = 1.0f / (k0 + dk * currParam);
                float rayMinW = min(prevRayW, currRayW);
                float rayMaxW = max(prevRayW, currRayW);
                if (rayMaxW >= sceneW && rayMinW <= sceneW + thickness) {
                    float lowParam = prevParam;
                    float highParam = currParam;
                    hitUv = sampleUv;
                    [loop]
                    for (uint refine = 0u; refine < refineSteps; ++refine) {
                        float midParam = 0.5f * (lowParam + highParam);
                        float2 midPos = UnpermutePixelPos(p0 + lineDelta * midParam, permuted);
                        uint2 midPixel = uint2(clamp(floor(midPos), float2(0.0f, 0.0f), u_screenSize.xy - 1.0f));
                        float2 midUv = (float2(midPixel) + 0.5f) * u_screenSize.zw;
                        float midSceneDepth = DepthTex.Load(int3(midPixel, 0));
                        if (!IsValidDepth(midSceneDepth)) {
                            lowParam = midParam;
                            continue;
                        }
                        float midRayW = 1.0f / (k0 + dk * midParam);
                        if (midRayW >= SceneViewDepth(midUv, midSceneDepth)) {
                            highParam = midParam;
                            hitUv = midUv;
                        } else {
                            lowParam = midParam;
                        }
                    }
                    hit = true;
                    hitConfidence = EdgeFade(hitUv) * (1.0f - smoothstep(0.45f, u_traceParams0.w, roughness));
                    break;
                }
                prevParam = currParam;
            }
        }
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
