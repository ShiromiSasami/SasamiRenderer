// Vulkan shadow-caster depth-only vertex shader (skinned), ported from
// Shaders/Native/Dx11/NativeShadowDepthSkinned_VS.hlsl. Applies bone-palette linear-blend skinning
// in model space before the same light-space transform used by the static depth path.

// Packed as a single 128-byte push constant block (Vulkan's guaranteed minimum size), matching
// VulkanShadowMap.cpp's local VulkanShadowPushConstants layout field-for-field.
struct ShadowPushConstants
{
    row_major float4x4 lightViewProjection;
    row_major float4x4 model;
};

// The register number here is arbitrary: [[vk::push_constant]] resources consume no descriptor
// set/binding at all, so b1 cannot collide with ShadowBoneCB's b0 below. b0 is reserved for
// ShadowBoneCB so it lands on binding 0 (see VulkanShadowMap.cpp's m_boneDescSetLayout, which has
// exactly one binding -- the bone matrix UBO -- in this pipeline's only descriptor set).
[[vk::push_constant]]
ConstantBuffer<ShadowPushConstants> g_shadow : register(b1);

// -fvk-b-shift 0 0 (ShaderCompilationService.cpp) shifts space0 'b' registers by 0, so this lands
// at Vulkan set=0, binding=0, matching VulkanShadowMap's dedicated skinned depth pipeline layout.
cbuffer ShadowBoneCB : register(b0)
{
    row_major float4x4 g_boneMatrices[128];
}

struct VSInput
{
    float3 position    : POSITION;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    r += w.x * mul(float4(pos, 1.0), g_boneMatrices[idx.x]);
    r += w.y * mul(float4(pos, 1.0), g_boneMatrices[idx.y]);
    r += w.z * mul(float4(pos, 1.0), g_boneMatrices[idx.z]);
    r += w.w * mul(float4(pos, 1.0), g_boneMatrices[idx.w]);
    return r;
}

float4 VSMain(VSInput input) : SV_POSITION
{
    float4 skinnedPos = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    return mul(mul(skinnedPos, g_shadow.model), g_shadow.lightViewProjection);
}
