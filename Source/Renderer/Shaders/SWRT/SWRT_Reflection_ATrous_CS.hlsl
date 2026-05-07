//
// SWRT_Reflection_ATrous_CS.hlsl
// Edge-aware spatial denoiser for noisy SWRT reflection radiance.
//
// Bindings reuse the legacy temporal root signature:
//   b0 = FilterCB
//   t0 = source reflection radiance (RGBA16F)
//   t1 = GBuffer normal/depth (RGBA16F, normal encoded as N*0.5+0.5, w=linear depth)
//   u0 = filtered reflection radiance (RGBA16F)
//

cbuffer FilterCB : register(b0)
{
    float g_stepWidth;
    uint  g_width;
    uint  g_height;
    float g_phiDepth;
};

Texture2D<float4> g_source      : register(t0);
Texture2D<float4> g_normalDepth : register(t1);
RWTexture2D<float4> g_output    : register(u0);

float3 DecodeNormal(float3 encodedNormal)
{
    return normalize(encodedNormal * 2.0f - 1.0f);
}

[numthreads(16, 16, 1)]
void CS_ReflectionATrous(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    const int2 pixel = int2(id.xy);
    const float4 centerRadiance = g_source.Load(int3(pixel, 0));
    const float4 centerGBuffer = g_normalDepth.Load(int3(pixel, 0));
    const float3 centerNormal = DecodeNormal(centerGBuffer.xyz);
    const float centerDepth = centerGBuffer.w;

    if (centerDepth <= 0.0f || centerRadiance.a <= 0.0f)
    {
        g_output[id.xy] = centerRadiance;
        return;
    }

    static const float kernel[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

    const int stepWidth = max(1, int(g_stepWidth + 0.5f));
    float4 sum = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (int oy = -2; oy <= 2; ++oy)
    {
        [unroll]
        for (int ox = -2; ox <= 2; ++ox)
        {
            const int2 samplePixel = clamp(pixel + int2(ox, oy) * stepWidth,
                                           int2(0, 0),
                                           int2(int(g_width) - 1, int(g_height) - 1));
            const float4 sampleRadiance = g_source.Load(int3(samplePixel, 0));
            const float4 sampleGBuffer = g_normalDepth.Load(int3(samplePixel, 0));
            const float3 sampleNormal = DecodeNormal(sampleGBuffer.xyz);
            const float sampleDepth = sampleGBuffer.w;

            const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 32.0f);
            const float depthWeight = exp(-abs(sampleDepth - centerDepth) / max(g_phiDepth, 1e-4f));
            const float kernelWeight = kernel[ox + 2] * kernel[oy + 2];
            const float alphaWeight = saturate(sampleRadiance.a * 4.0f);
            const float w = kernelWeight * normalWeight * depthWeight * alphaWeight;

            sum += sampleRadiance * w;
            weightSum += w;
        }
    }

    g_output[id.xy] = (weightSum > 1e-5f) ? (sum / weightSum) : centerRadiance;
}
