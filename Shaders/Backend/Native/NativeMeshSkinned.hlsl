// Chan 2018 micro-shadowing, shared verbatim with the D3D12 lighting passes so the two cannot
// drift apart. Resolved through DXC's -I <shader root> (ShaderCompilationService), the same
// mechanism every other shared header under Shaders/ relies on.
#include "Shared/Common/MicroShadowing.hlsli"
#include "../../Native/NativePbr.hlsli"
#include "../../Native/NativeShadow.hlsli"
#include "../../Native/NativeTonemap.hlsli"
#include "../../Native/NativeLighting.hlsli"

struct MeshPushConstants
{
    // Per-draw data only; view/projection and the light moved out to g_frame below since they are
    // the same for every draw this frame (VulkanNativeMeshPushConstants must fit Vulkan's
    // guaranteed 128-byte push constant minimum -- see its static_assert).
    row_major float4x4 model;
    float4 baseColor;
    // xyz = emissive, w = roughness.
    float4 emissiveRoughness;
    // x = metallic, yzw = unused.
    float4 materialParams;
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

// Per-frame constants shared by every draw this frame, matching VulkanNativeMeshFrameConstants
// (VulkanGraphicsDeviceUtils.h) field-for-field. b1/space1 -> Vulkan set=1, binding=1 (binding=0
// in that set is g_bones above; see the skinned bone descriptor set layout in
// VulkanGraphicsDeviceSkinnedMesh.cpp, which now carries both bindings).
struct FrameConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    // x = shadow depth bias, y = shadow normal bias, z = shadow map texel world size,
    // w = shadow map resolution, or 0 if shadows are disabled/unavailable this frame.
    float4 shadowParams;
    // x = 1 when a refreshed runtime-AO texture is bound at t4 this frame (0 => the white dummy,
    // so runtimeAo reads 1.0 and drops out), yz = inverse render resolution for the screen-space
    // lookup, w = tonemap exposure (RenderSettings::exposure). Mirrors DirectX 11's u_featureFlags.z / u_featureFlags2.zw.
    float4 aoParams;
    // xyz = direction the light travels (shaders negate it to get l), w = intensity.
    float4 lightDirIntensity;
    // rgb = light colour, w = 1 when the ambient cube at t1 is bound.
    float4 lightColor;
    // xyz = camera world position, w = 1 when image-based ambient is enabled.
    float4 cameraPosFlags;
};
ConstantBuffer<FrameConstants> g_frame : register(b1, space1);

Texture2D<float4> g_albedo  : register(t0);
SamplerState      g_sampler : register(s0);

// Same low-resolution environment cube the static mesh path uses, bound from the same descriptor
// set (the backend already supplied it for skinned draws; only this shader was still ignoring it,
// which left skinned meshes lit by the flat constant while static ones picked up the environment).
TextureCube<float4> g_ambientCube    : register(t1);
SamplerState        g_ambientSampler : register(s1);

// Material AO (t2/s2 -- see GetOrCreateNativeMeshTextureDescriptorSet). Unlike the static mesh
// path, the skinned loader (ModelLoaderSkinned.cpp) only ever fills this slot from glTF's
// dedicated occlusionTexture and never substitutes metallic-roughness into it, so whenever a
// texture is bound here its R channel is always real ambient occlusion (see
// RhiBackendSkinnedMeshDrawDesc::occlusionSlotHasAo). Binds the 1x1 white dummy when there is no
// occlusion texture, so .r reads as 1.0 (no occlusion), matching pre-AO rendering exactly.
Texture2D<float4> g_occlusion       : register(t2);
SamplerState       g_occlusionSampler : register(s2);

// Directional shadow map: depth-only, sampled with hardware PCF comparison (see SampleShadow
// below, ported from NativeShadow.hlsli). t3/s3 -- confirmed free of the t0-t2/s0-s2 slots above
// (see GetOrCreateNativeMeshTextureDescriptorSet).
Texture2D                 g_shadowMap     : register(t3);
SamplerComparisonState    g_shadowSampler : register(s3);

// Runtime (screen-space) ambient occlusion -- DirectX 12's RuntimeAOTex, produced here by
// VulkanAmbientOcclusion (depth+normal prepass -> SSAO -> blur, all before this pass runs). t4/s4,
// the next free pair after the shadow map, in the same set-0 texture set the static path uses, so
// both shaders see the identical binding. Gated by g_frame.aoParams.x: when it is 0 the white
// dummy is bound and runtimeAo is 1.0, which is exactly pre-SSAO behaviour.
Texture2D<float4> g_runtimeAo        : register(t4);
SamplerState      g_runtimeAoSampler : register(s4);

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
    float3 worldPos : TEXCOORD1;
    float3 worldN   : TEXCOORD2;
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
    float3 skinnedNormal = SkinNormal(input.normal, input.boneIndices, input.boneWeights);
    float4 worldPos = mul(skinnedPos, g_mesh.model);
    output.position = mul(worldPos, g_frame.viewProjection);
    output.normal = normalize(skinnedNormal);
    output.color = input.color * g_mesh.baseColor;
    output.uv = input.uv;
    output.worldPos = worldPos.xyz;
    output.worldN = normalize(mul(skinnedNormal, (float3x3)g_mesh.model));
    return output;
}

// Duplicated verbatim from NativeMesh.hlsl's PSMain: Vulkan pipelines bake VS+PS into one
// monolithic VkPipeline object, so the compiled static-mesh PS module can't be rebound into
// this pipeline the way DX11 reuses the same ID3D11PixelShader COM object across draws.
float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.worldN);
    float3 v = normalize(g_frame.cameraPosFlags.xyz - input.worldPos);
    float3 l = normalize(-g_frame.lightDirIntensity.xyz);

    // Mirrors DX12's DeferredLighting_PS.hlsl: rawAo = saturate(materialAo * runtimeAo).
    // runtimeAo is now real on this path too (VulkanAmbientOcclusion, generated earlier in the
    // frame), looked up by screen position rather than UV because it is a screen-space term.
    // SV_POSITION.xy is already in top-left-origin framebuffer coordinates -- the viewport's
    // negative height applies the same flip to this pass and to the AO chain -- so dividing by the
    // render resolution lands on the matching AO texel.
    float materialAo = g_occlusion.Sample(g_occlusionSampler, input.uv).r;
    float runtimeAo = 1.0f;
    if (g_frame.aoParams.x > 0.5f) {
        float2 aoUv = input.position.xy * g_frame.aoParams.yz;
        runtimeAo = g_runtimeAo.Sample(g_runtimeAoSampler, aoUv).r;
    }
    float ao = NativeResolveAo(materialAo, runtimeAo);

    float4 texSample = g_albedo.Sample(g_sampler, input.uv);

    // Only the direct-light term is shadowed, never ambient (see NativeShadeSurface). SampleShadow
    // early-outs to 1.0 when shadowParams.w == 0 (shadows disabled or unavailable this frame).
    float shadowTerm = SampleShadow(g_shadowMap, g_shadowSampler, input.worldPos, n, g_frame.lightViewProjection, g_frame.shadowParams);

    float roughness = saturate(g_mesh.emissiveRoughness.w);
    float metallic = saturate(g_mesh.materialParams.x);

    // Prefers the real environment cube once one is bound, otherwise the analytic hemisphere.
    bool ambientEnabled = g_frame.cameraPosFlags.w > 0.5f;
    float3 ambientRadiance = (g_frame.lightColor.w > 0.5f)
        ? g_ambientCube.Sample(g_ambientSampler, n).rgb
        : NativeHemisphereAmbient(n);

    float4 surface = input.color * texSample;
    float3 lit = NativeShadeSurface(surface.rgb, metallic, roughness, n, v, l, g_frame.lightColor.rgb,
                                    g_frame.lightDirIntensity.w, shadowTerm, ao, ambientEnabled,
                                    ambientRadiance, g_mesh.emissiveRoughness.rgb);
    lit = NativeTonemap(lit, g_frame.aoParams.w);
    lit = pow(saturate(lit), 1.0f / 2.2f);
    return float4(lit, surface.a);
}
