// ShadowVSM_GaussBlur_CS.hlsl
// Separable 7-tap Gaussian blur (sigma=1.5) for VSM shadow maps.
// H pass: reads vsmMap, writes vsmMapTemp.
// V pass: reads vsmMapTemp, writes vsmMap.
// Dispatched only when VSM blur is enabled.

cbuffer BlurConstants : register(b0)
{
    uint g_width;
    uint g_height;
    uint g_sliceIndex;  // which cascade slice to blur
    uint g_pad;
};

Texture2DArray<float2>   g_srcTex : register(t0);
RWTexture2DArray<float2> g_dstTex : register(u0);

// 7-tap Gaussian weights, sigma=1.5, kernel radius=3.
// Weights are pre-normalized.
static const float kWeights[7] = {
    0.03125f, 0.109375f, 0.21875f, 0.28125f, 0.21875f, 0.109375f, 0.03125f
};

[numthreads(8, 8, 1)]
void CS_BlurH(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) return;

    float2 result = float2(0.0f, 0.0f);
    [unroll]
    for (int i = -3; i <= 3; ++i)
    {
        int sx = clamp((int)id.x + i, 0, (int)g_width - 1);
        result += kWeights[i + 3] * g_srcTex.Load(int4(sx, id.y, g_sliceIndex, 0));
    }
    g_dstTex[uint3(id.x, id.y, g_sliceIndex)] = result;
}

[numthreads(8, 8, 1)]
void CS_BlurV(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) return;

    float2 result = float2(0.0f, 0.0f);
    [unroll]
    for (int i = -3; i <= 3; ++i)
    {
        int sy = clamp((int)id.y + i, 0, (int)g_height - 1);
        result += kWeights[i + 3] * g_srcTex.Load(int4(id.x, sy, g_sliceIndex, 0));
    }
    g_dstTex[uint3(id.x, id.y, g_sliceIndex)] = result;
}
