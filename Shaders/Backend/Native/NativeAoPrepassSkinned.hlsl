// Vulkan ambient-occlusion depth + world-normal prepass, skinned variant: vertex shader only.
//
// Emits the same PSInput as NativeAoPrepass.hlsl's VSMain so the backend can pair both prepass
// pipelines with that file's single compiled PSMain instead of duplicating the encode.
//
// Skinning matches NativeMeshSkinned.hlsl's SkinPosition/SkinNormal exactly (linear blend, normals
// transformed by each bone's rotation/scale block), so a skinned occluder's AO normals agree with
// the normals the forward pass shades it with.

// Same 128-byte push constant block as the static prepass (VulkanAoPrepassPushConstants in
// VulkanAmbientOcclusion.cpp). The register number is arbitrary: [[vk::push_constant]] resources
// consume no descriptor set/binding, so b1 here cannot collide with the bone UBO's b0 below --
// the same arrangement NativeShadowDepthSkinned_VS.hlsl uses.
struct AoPrepassPushConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<AoPrepassPushConstants> g_aoPrepass : register(b1);

// -fvk-b-shift 0 0 (ShaderCompilationService.cpp) leaves space0 'b' registers unshifted, so this
// lands at Vulkan set=0, binding=0 -- the single dynamic-UBO binding in the skinned prepass
// pipeline's only descriptor set (see VulkanAmbientOcclusion's bone descriptor set layout).
cbuffer AoPrepassBoneCB : register(b0)
{
    row_major float4x4 g_boneMatrices[128];
}

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : TEXCOORD0;
};

float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    const float4 p = float4(pos, 1.0);
    return w.x * mul(p, g_boneMatrices[idx.x])
         + w.y * mul(p, g_boneMatrices[idx.y])
         + w.z * mul(p, g_boneMatrices[idx.z])
         + w.w * mul(p, g_boneMatrices[idx.w]);
}

float3 SkinNormal(float3 n, uint4 idx, float4 w)
{
    return w.x * mul(n, (float3x3)g_boneMatrices[idx.x])
         + w.y * mul(n, (float3x3)g_boneMatrices[idx.y])
         + w.z * mul(n, (float3x3)g_boneMatrices[idx.z])
         + w.w * mul(n, (float3x3)g_boneMatrices[idx.w]);
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float4 skinnedPos = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    const float3 skinnedNormal = SkinNormal(input.normal, input.boneIndices, input.boneWeights);
    const float4 worldPos = mul(skinnedPos, g_aoPrepass.model);
    output.position = mul(worldPos, g_aoPrepass.viewProjection);
    output.worldN = mul(skinnedNormal, (float3x3)g_aoPrepass.model);
    return output;
}
