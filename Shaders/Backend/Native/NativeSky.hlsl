// NativeSky.hlsl -- the renderer's analytic sky for the Vulkan native path.
//
// Same job and same source of truth as Shaders/Native/Dx11/NativeSky_PS.hlsl: both include the one
// ComputeSkyColor that the D3D12 procedural sky, the SWRT miss path and the water's environment
// reflection also use, so no surface in the renderer can disagree about what the sky looks like.
//
// The include is relative to this file rather than to the shader root. Shaders/Backend/Native and
// Shaders/Native/Dx11 are both two levels down, so the two backends happen to spell it identically.
#include "../../Effects/Sky/ProceduralSky/ProceduralSky.hlsli"
#include "../../Native/NativeTonemap.hlsli"

// Push constants rather than a uniform buffer: the whole payload is 116 bytes, inside the 128 that
// Vulkan guarantees, so this needs no descriptor set of its own, no allocation and no per-frame
// update logic. (The environment cubemap below does need its own descriptor set, but that set holds
// no per-frame data -- it is only rebound when the bound texture changes.)
struct SkyPushConstants
{
    row_major float4x4 invViewProjection;
    float4             cameraPos;     // xyz
    float4             sunDirection;  // xyz = light-to-surface, w = intensity
    float4             sunColor;      // rgb, w = tonemap exposure (RenderSettings::exposure)
    float              hasEnvironmentCube; // 1 = sample g_envCube, 0 = fall back to ComputeSkyColor()
};
[[vk::push_constant]] SkyPushConstants g_sky;

// HDR environment cubemap for the sky. Bound to a 1x1 dummy cube (see
// VulkanGraphicsDeviceSky.cpp's EnsureNativeSkyResources) when no HDR environment is loaded, so this
// declaration is always satisfied -- g_sky.hasEnvironmentCube is what actually decides whether it
// is sampled, keeping ComputeSkyColor() as the fallback for samples with no HDR sky set.
TextureCube  g_envCube    : register(t0);
SamplerState g_envSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Reverse-Z: near = 1.0, far = 0.0, so the near point takes z = 1 and the far point z = 0.
    const float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    const float4 nearH = mul(float4(ndc, 1.0f, 1.0f), g_sky.invViewProjection);
    const float4 farH  = mul(float4(ndc, 0.0f, 1.0f), g_sky.invViewProjection);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float3 rayDir = normalize((farH.xyz / farH.w) - (nearH.xyz / nearH.w));

    if (g_sky.hasEnvironmentCube > 0.5f) {
        // Single mip (see the sampler's maxLod = 0), so an explicit LOD-0 sample avoids relying on
        // implicit screen-space derivatives across a full-screen triangle.
        // The cube holds linear HDR radiance well above 1.0, but this pass writes straight into an
        // 8-bit UNORM swapchain image, so handing it over untouched clips every bright region to
        // flat white. Apply the same ACES + gamma that NativeMesh.hlsl uses for surfaces, so
        // sky and geometry end up in the same display space. The procedural fallback below is
        // already display-ready and must NOT go through this.
        float3 skyRadiance = g_envCube.SampleLevel(g_envSampler, rayDir, 0.0f).rgb;
        skyRadiance = NativeTonemap(skyRadiance, g_sky.sunColor.w);
        skyRadiance = pow(saturate(skyRadiance), 1.0f / 2.2f);
        return float4(skyRadiance, 1.0f);
    }

    const float3 toSun = normalize(-g_sky.sunDirection.xyz);
    return float4(ComputeSkyColor(rayDir, toSun, g_sky.sunColor.rgb, g_sky.sunDirection.w), 1.0f);
}
