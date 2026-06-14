cbuffer BlurCB : register(b0)
{
    row_major float4x4 u_cameraPV;
    row_major float4x4 u_cameraInvPV;
    float4 u_ssaoParams;
    float4 u_screenSize;
}

Texture2D<float4> t_ssao   : register(t0);
Texture2D<float>  t_depth  : register(t1);
Texture2D<float4> t_normal : register(t2);
SamplerState      s_point  : register(s0);

struct PSInput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float3 DecodeNormal(float2 uv)
{
    float3 encoded = t_normal.SampleLevel(s_point, uv, 0).xyz;
    float3 normal = encoded * 2.0f - 1.0f;
    float lenSq = dot(normal, normal);
    if (lenSq <= 1e-5f) {
        return float3(0.0f, 1.0f, 0.0f);
    }
    return normalize(normal);
}

float4 PSMain(PSInput input) : SV_Target
{
    const float2 uv = input.uv;
    const float2 texelSize = u_screenSize.zw;
    const float centerDepth = t_depth.SampleLevel(s_point, uv, 0).r;

    if (centerDepth >= 1.0f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    const float3 centerNormal = DecodeNormal(uv);
    static const float kernel[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };

    float result = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            const float2 sampleUv = uv + float2((float)x, (float)y) * texelSize;
            const float sampleDepth = t_depth.SampleLevel(s_point, sampleUv, 0).r;
            const float sampleAo = t_ssao.SampleLevel(s_point, sampleUv, 0).r;
            const float3 sampleNormal = DecodeNormal(sampleUv);

            const float depthDiff = abs(centerDepth - sampleDepth);
            const float depthWeight = exp(-depthDiff * 32.0f);

            const float normalDiff = 1.0f - saturate(dot(centerNormal, sampleNormal));
            const float normalWeight = exp(-normalDiff * 24.0f);

            const float w = kernel[x + 2] * kernel[y + 2] * depthWeight * normalWeight;
            result += sampleAo * w;
            weightSum += w;
        }
    }

    const float blurred = result / max(weightSum, 1e-4f);
    return float4(blurred, blurred, blurred, 1.0f);
}
