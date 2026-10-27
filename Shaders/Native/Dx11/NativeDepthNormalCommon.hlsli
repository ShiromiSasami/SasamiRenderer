// Shared constant buffer and vertex/pixel I/O for the DirectX 11 native depth+normal prepass.
//
// Why this prepass exists at all: the D3D12 path feeds its SSAO generator the deferred GBuffer's
// depth and normal attachments (see SSAORenderPass::Setup, which Read()s "SceneDepth" and
// "GBufferNormal"). The native DX11 path is forward-shaded and has no GBuffer, so the two inputs
// SSAO_PS.hlsl needs have to be produced on purpose. Rendering them is preferred over
// reconstructing normals from depth derivatives: TODO.md's Vulkan SSAO entry records that
// derivative-reconstructed normals were not accurate enough there.
//
// Field order matches Dx11DepthNormalConstants on the C++ side -- do not reorder.

cbuffer DepthNormalCB : register(b0)
{
    row_major float4x4 u_viewProjection;
    row_major float4x4 u_model;
}

cbuffer DepthNormalBoneCB : register(b1)
{
    row_major float4x4 u_boneMatrices[128];
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
};

struct SkinnedVSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
};
