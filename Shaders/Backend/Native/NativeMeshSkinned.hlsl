struct MeshPushConstants
{
    row_major float4x4 modelViewProjection;
    float4 baseColor;
    float4 lightDirIntensity;
    float4 lightColor;
    float4 emissiveRoughness;
};

[[vk::push_constant]]
ConstantBuffer<MeshPushConstants> g_mesh : register(b0);

// DXC's Vulkan register-shift table (ShaderCompilationService.cpp) only remaps space0;
// space1 registers pass through unshifted, so b0/space1 lands at Vulkan set=1, binding=0.
struct BoneMatrices
{
    row_major float4x4 boneMatrices[128];
};
ConstantBuffer<BoneMatrices> g_bones : register(b0, space1);

Texture2D<float4> g_albedo  : register(t0);
SamplerState      g_sampler : register(s0);

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 uv          : TEXCOORD;
    uint4  boneIndices : JOINTS_0;
    float4 boneWeights : WEIGHTS_0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

float4 SkinPosition(float3 pos, uint4 idx, float4 w)
{
    float4 p = float4(pos, 1.0f);
    return w.x * mul(p, g_bones.boneMatrices[idx.x])
         + w.y * mul(p, g_bones.boneMatrices[idx.y])
         + w.z * mul(p, g_bones.boneMatrices[idx.z])
         + w.w * mul(p, g_bones.boneMatrices[idx.w]);
}

float3 SkinNormal(float3 n, uint4 idx, float4 w)
{
    return w.x * mul(n, (float3x3)g_bones.boneMatrices[idx.x])
         + w.y * mul(n, (float3x3)g_bones.boneMatrices[idx.y])
         + w.z * mul(n, (float3x3)g_bones.boneMatrices[idx.z])
         + w.w * mul(n, (float3x3)g_bones.boneMatrices[idx.w]);
}

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 skinnedPos = SkinPosition(input.position, input.boneIndices, input.boneWeights);
    output.position = mul(skinnedPos, g_mesh.modelViewProjection);
    output.normal = normalize(SkinNormal(input.normal, input.boneIndices, input.boneWeights));
    output.color = input.color * g_mesh.baseColor;
    output.uv = input.uv;
    return output;
}

// Duplicated verbatim from NativeMesh.hlsl's PSMain: Vulkan pipelines bake VS+PS into one
// monolithic VkPipeline object, so the compiled static-mesh PS module can't be rebound into
// this pipeline the way DX11 reuses the same ID3D11PixelShader COM object across draws.
float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float3 l = normalize(-g_mesh.lightDirIntensity.xyz);
    float ndotl = saturate(dot(n, l));
    float ambient = 0.16f;
    float4 texSample = g_albedo.Sample(g_sampler, input.uv);
    float4 surface = input.color * texSample;
    float3 lit = surface.rgb * g_mesh.lightColor.rgb * (ambient + ndotl * g_mesh.lightDirIntensity.w);
    lit += g_mesh.emissiveRoughness.rgb;
    lit = lit / (lit + 1.0f);
    lit = pow(saturate(lit), 1.0f / 2.2f);
    return float4(lit, surface.a);
}
