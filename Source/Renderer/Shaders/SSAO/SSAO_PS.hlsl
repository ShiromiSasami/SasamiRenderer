cbuffer SSAOCB : register(b0)
{
    row_major float4x4 u_cameraPV;
    row_major float4x4 u_cameraInvPV;
    float4 u_ssaoParams;   // x: radius, y: bias, z: intensity, w: thickness
    float4 u_screenSize;   // x: width, y: height, z: 1/width, w: 1/height
    float4 u_cameraPos;    // xyz: camera world position, w: quality index
}

Texture2D<float>  DepthTex   : register(t0);
Texture2D<float4> NormalTex  : register(t1);
SamplerState      PointClamp : register(s0);

#include "SSAO_Sampling.hlsli"


float4 PSMain(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float depth = DepthTex.SampleLevel(PointClamp, uv, 0).r;
    if (depth >= 1.0f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 normal = DecodeNormal(uv);
    float3 toCamera = u_cameraPos.xyz - worldPos;
    if (dot(normal, toCamera) < 0.0f)
    {
        normal = -normal;
    }

    const float radius = max(u_ssaoParams.x, 0.01f);
    const float bias = max(u_ssaoParams.y, 1e-4f);
    const float intensity = max(u_ssaoParams.z, 0.0f);
    const float thickness = max(u_ssaoParams.w, 1e-4f);
    int qualityIndex = (int)round(u_cameraPos.w);
    qualityIndex = max(0, min(qualityIndex, 2));
    const uint quality = (uint)qualityIndex;
    const uint sampleCount = ResolveSampleCount(quality);

    float noise = Hash12(floor(pos.xy));
    float angle = noise * PI * 2.0f;
    float3 tangent = BuildTangent(normal, angle);
    float3 bitangent = normalize(cross(normal, tangent));

    float occlusion = 0.0f;
    float weightSum = 0.0f;

    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        float3 hemisphereDir = g_ssaoKernel[i];
        float3 sampleDir = tangent * hemisphereDir.x +
                           bitangent * hemisphereDir.y +
                           normal * hemisphereDir.z;
        sampleDir = normalize(sampleDir);

        float sampleRadius = radius * ResolveSampleRadius(i, sampleCount);
        float3 samplePos = worldPos + sampleDir * sampleRadius;
        float4 sampleClip = mul(float4(samplePos, 1.0f), u_cameraPV);
        if (sampleClip.w <= 1e-5f)
        {
            continue;
        }

        float3 sampleNdc = sampleClip.xyz / sampleClip.w;
        float2 sampleUv = sampleNdc.xy * float2(0.5f, -0.5f) + 0.5f;
        if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
        {
            continue;
        }

        float sceneDepth = DepthTex.SampleLevel(PointClamp, sampleUv, 0).r;
        if (sceneDepth >= 1.0f)
        {
            continue;
        }

        float3 scenePos = ReconstructWorldPos(sampleUv, sceneDepth);
        float3 sceneDelta = scenePos - worldPos;
        float sceneDistance = length(sceneDelta);
        if (sceneDistance <= 1e-5f)
        {
            continue;
        }

        float projectedDistance = dot(sceneDelta, sampleDir);
        float depthDelta = sampleRadius - projectedDistance;
        float rangeWeight = saturate(1.0f - sceneDistance / (radius * 1.5f));
        float horizonWeight = saturate(dot(normal, sampleDir));

        float3 sceneNormal = DecodeNormal(sampleUv);
        float normalWeight = saturate(dot(sceneNormal, -sampleDir));
        normalWeight = lerp(0.35f, 1.0f, normalWeight);

        float occlusionWeight = smoothstep(bias, bias + thickness, depthDelta);
        occlusion += occlusionWeight * rangeWeight * horizonWeight * normalWeight;
        weightSum += max(horizonWeight, 1e-4f);
    }

    float visibility = 1.0f;
    if (weightSum > 0.0f)
    {
        visibility = saturate(1.0f - (occlusion / weightSum) * intensity);
    }
    return float4(visibility, visibility, visibility, 1.0f);
}
