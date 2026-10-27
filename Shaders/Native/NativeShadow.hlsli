#pragma once

// 3x3 PCF shadow sampling against a reverse-Z depth map, used by the DirectX 11
// and Vulkan native mesh pixel shaders (FXC and DXC compatible).

// shadowParams: x = depth bias, y = normal bias, z = texel world size, w = shadow map resolution
// (0 = shadows disabled).
float SampleShadow(Texture2D shadowMap, SamplerComparisonState cmp, float3 worldPos,
                    float3 worldNormal, float4x4 lightViewProjection, float4 shadowParams)
{
    if (shadowParams.w == 0.0)
    {
        return 1.0;
    }

    // Push the sample point along the surface normal by one texel's worth of world space to
    // avoid self-shadowing acne on grazing-angle surfaces.
    float3 biasedPos = worldPos + worldNormal * (shadowParams.y * shadowParams.z);

    float4 lp = mul(float4(biasedPos, 1.0), lightViewProjection);
    lp.xyz /= lp.w;

    // NDC -> texture space, flipping Y because D3D's viewport/texture origin is top-left while
    // NDC Y points up.
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;

    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1.0;
    }

    // Reverse-Z: near plane is at depth 1.0, so an unoccluded point is at least as close (>=) as
    // the stored depth once the bias is added.
    float compareDepth = lp.z + shadowParams.x;

    float texel = 1.0 / shadowParams.w;
    float sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            sum += shadowMap.SampleCmpLevelZero(cmp, uv + offset, compareDepth);
        }
    }

    return sum / 9.0;
}
