// Vulkan shadow-caster depth-only vertex shader (static, non-skinned), ported from
// Shaders/Native/Dx11/NativeShadowDepth_VS.hlsl. Only position is needed since the shadow map has
// no pixel shader output beyond depth.

// Packed as a single 128-byte push constant block (Vulkan's guaranteed minimum size), matching
// VulkanShadowMap.cpp's local VulkanShadowPushConstants layout field-for-field.
struct ShadowPushConstants
{
    row_major float4x4 lightViewProjection;
    row_major float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<ShadowPushConstants> g_shadow : register(b0);

struct VSInput
{
    float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    return mul(mul(float4(input.position, 1.0), g_shadow.model), g_shadow.lightViewProjection);
}
