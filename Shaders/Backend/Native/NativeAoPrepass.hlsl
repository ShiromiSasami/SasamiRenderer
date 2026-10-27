// Vulkan ambient-occlusion depth + world-normal prepass (static, non-skinned geometry).
//
// The native Vulkan path is forward-shaded and owns no G-Buffer, so the SSAO generator has no
// normal texture to read -- and reconstructing normals from depth derivatives is too imprecise to
// be worth it (see TODO.md's 2026-09-17 retrospective). This pass therefore produces exactly the
// two inputs DirectX 12's SSAO pass reads out of its G-Buffer, and nothing else:
//
//   * depth, in this renderer's reverse-Z convention (near = 1.0, far = 0.0), and
//   * the world-space normal, encoded N * 0.5 + 0.5 -- byte-for-byte the same encoding as
//     Shaders/Raster/Geometry/OpaqueGBuffer/GBufferEncode.hlsli writes into SV_TARGET1, so the
//     shared SSAO_NormalDecode.hlsli decode in SSAO_PS.hlsl/SSAO_Blur_PS.hlsl applies unchanged.
//
// PSMain is deliberately shared with the skinned variant (NativeAoPrepassSkinned.hlsl supplies
// only its own VSMain): both vertex shaders emit the same PSInput, so the backend compiles this
// pixel shader once and pairs the one module with both prepass pipelines.

// Single 128-byte push constant block -- Vulkan's guaranteed minimum, the same budget
// VulkanShadowPushConstants and VulkanNativeMeshPushConstants live within. Matches
// VulkanAmbientOcclusion.cpp's VulkanAoPrepassPushConstants field-for-field.
struct AoPrepassPushConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 model;
};

[[vk::push_constant]]
ConstantBuffer<AoPrepassPushConstants> g_aoPrepass : register(b0);

// Only position and normal are read, but the vertex buffer is the main mesh path's interleaved
// 48-byte vertex, so the pipeline declares the same stride and leaves colour/UV unconsumed (same
// arrangement as NativeShadowDepth_VS.hlsl).
struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float4 worldPos = mul(float4(input.position, 1.0), g_aoPrepass.model);
    output.position = mul(worldPos, g_aoPrepass.viewProjection);
    // Normals are transformed by the model matrix's rotation/scale block only, exactly as
    // NativeMesh.hlsl's VSMain does for its worldN; non-uniform scale is left uncorrected here
    // for the same reason it is there (no inverse-transpose is plumbed through).
    output.worldN = mul(input.normal, (float3x3)g_aoPrepass.model);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // A degenerate interpolated normal (zero-length vertex normal, or a bone palette that
    // cancelled out in the skinned variant) would make normalize() produce NaN and poison the
    // whole SSAO kernel. Write the neutral 0.5 grey instead, which SSAO_NormalDecode.hlsli's
    // lenSq <= 1e-5 branch already turns back into its (0,1,0) fallback.
    const float lenSq = dot(input.worldN, input.worldN);
    if (lenSq <= 1e-8)
    {
        return float4(0.5, 0.5, 0.5, 1.0);
    }
    const float3 n = input.worldN * rsqrt(lenSq);
    return float4(n * 0.5 + 0.5, 1.0);
}
