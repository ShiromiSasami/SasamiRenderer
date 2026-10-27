// Shared constant buffer layout, vertex/pixel I/O structs, and resource bindings for the
// DirectX 11 native mesh pipeline (static and skinned variants, opaque + shadow-casting).
// Field order matches the layout the C++ side uploads via Dx11MeshConstants -- do not reorder.

cbuffer MeshCB : register(b0)
{
    row_major float4x4 u_viewProjection;
    row_major float4x4 u_model;
    float4 u_baseColor;
    float4 u_emissiveRoughness;   // rgb = emissive, w = roughness
    float4 u_lightDirIntensity;   // xyz = sun direction (pointing from the sun), w = intensity
    float4 u_lightColor;
    float4 u_cameraPosMetallic;   // xyz = camera world position, w = metallic
    // z = a runtime (screen-space) AO texture was produced this frame, not merely requested: it
    // gates the RuntimeAOTex sample the way u_shadowParams.w gates the shadow lookup.
    float4 u_featureFlags;        // x = has albedo texture, y = ibl, z = ssao, w = shadow
    // zw = 1/renderWidth, 1/renderHeight, which turns SV_POSITION into the screen UV RuntimeAOTex
    // is indexed by. Packed here instead of growing the constant buffer for two floats.
    float4 u_featureFlags2;       // x = post process (tonemap + gamma), y = ambient cube bound
    row_major float4x4 u_lightViewProjection;
    float4 u_shadowParams;        // x = depth bias, y = normal bias, z = texel world size, w = shadow map resolution (0 = none)
    float4 u_exposureParams;      // x = exposure (RenderSettings::exposure)
}

cbuffer SkinnedBoneCB : register(b1)
{
    row_major float4x4 u_boneMatrices[128];
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct SkinnedVSInput
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
    float3 worldN   : NORMAL;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

Texture2D AlbedoTex               : register(t0);
Texture2D ShadowMap               : register(t1);
// Material occlusion. Only holds a real AO texture when occlusionSlotHasAo confirms the bound
// slot's R channel really is ambient occlusion -- see RhiBackendMeshDrawDesc::occlusionSlotHasAo
// and RhiBackendSkinnedMeshDrawDesc::occlusionSlotHasAo (this shader is shared by both the static
// and skinned draw paths). Otherwise Dx11GraphicsDevice binds a 1x1 white dummy here, so .r reads
// back 1.0 (no occlusion) instead of an unrelated metallic-roughness channel.
Texture2D OcclusionTex            : register(t2);
// Skybox-built ambient irradiance cube, read only when u_featureFlags2.y confirms Dx11GraphicsDevice
// bound a real cube (it arrives asynchronously once the HDR skybox source loads; see
// Skybox::BuildNativeCubemap). Reuses LinearWrap rather than a dedicated sampler: SM4+ cube sampling
// is always seamless regardless of the sampler's address mode, so a separate clamp sampler buys
// nothing here.
TextureCube AmbientCube           : register(t3);
// Screen-space ambient occlusion for the current frame, full render resolution, 1.0 = unoccluded.
// Dx11SsaoGenerator writes it from the depth and normals Dx11DepthNormalPrepass renders; it is the
// native path's equivalent of the D3D12 deferred shader's RuntimeAOTex (register t9 there). Only
// sampled when u_featureFlags.z confirms it was produced; otherwise Dx11GraphicsDevice binds the
// same 1x1 white dummy OcclusionTex falls back to, so .r reads 1.0.
Texture2D RuntimeAOTex            : register(t4);
SamplerState LinearWrap           : register(s0);
SamplerComparisonState ShadowCmp  : register(s1);
