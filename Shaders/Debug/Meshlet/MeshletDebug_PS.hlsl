// MeshletDebug_PS.hlsl
// Colors each meshlet with a unique color so meshlet boundaries are visible.
//
// This PS is paired with the standard VERTEX shader (RenderPipelineStateCache builds
// the "MeshletDebug" PSO from the regular VS + this PS), NOT with MS_Meshlet.  A
// vertex shader cannot supply a per-primitive attribute, so the meshlet index is
// derived from SV_PrimitiveID.  That is exact for the current builder, which chunks
// the index buffer into sequential runs of kMaxTrianglesPerMeshlet triangles
// (MeshletBuffer.cpp), so primitive N belongs to meshlet N / kMaxTrianglesPerMeshlet.
//
// The divisor is pulled from the shared header rather than hardcoded so it cannot
// drift away from the CPU-side chunk size.  If the builder is ever switched to a
// non-sequential clustering (e.g. meshoptimizer), this derivation stops being valid
// and the debug view must move to a mesh-shader PSO reading MESHLET_INDEX, which
// MeshShader_MS.hlsl already exports.
//
// GBuffer outputs match PBR_PS.hlsl so this PSO binds to the same 5 RTVs used by
// LightingRenderNode without any RTV count mismatch.

#include "Shared/Common/MeshletConstants.hlsli"

// 16-color palette with maximum perceptual separation.
// Colours alternate between warm/cool and high/low saturation so adjacent
// meshlets (which are spatially close) tend to receive contrasting colours.
static const float3 kMeshletColors[16] =
{
    float3(0.94f, 0.20f, 0.20f), // 0  vivid red
    float3(0.20f, 0.75f, 0.95f), // 1  sky blue
    float3(0.20f, 0.85f, 0.30f), // 2  lime green
    float3(0.95f, 0.80f, 0.10f), // 3  golden yellow
    float3(0.85f, 0.20f, 0.90f), // 4  violet
    float3(0.10f, 0.80f, 0.70f), // 5  teal
    float3(0.95f, 0.50f, 0.10f), // 6  orange
    float3(0.40f, 0.20f, 0.95f), // 7  indigo
    float3(0.95f, 0.55f, 0.75f), // 8  pink
    float3(0.30f, 0.95f, 0.60f), // 9  mint
    float3(0.70f, 0.40f, 0.10f), // 10 brown
    float3(0.60f, 0.95f, 0.20f), // 11 yellow-green
    float3(0.10f, 0.40f, 0.90f), // 12 cobalt
    float3(0.95f, 0.30f, 0.55f), // 13 crimson-rose
    float3(0.50f, 0.95f, 0.90f), // 14 aqua
    float3(0.75f, 0.20f, 0.40f), // 15 burgundy
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

struct PSOutput
{
    float4 color    : SV_TARGET0; // SceneColor
    float4 albedo   : SV_TARGET1; // GBufferAlbedo
    float4 normal   : SV_TARGET2; // GBufferNormal
    float4 material : SV_TARGET3; // GBufferMaterial
    float4 emissive : SV_TARGET4; // GBufferEmissive
};

PSOutput PSMain(PSInput input, uint primitiveId : SV_PrimitiveID)
{
    const uint meshletIdx    = primitiveId / kMaxTrianglesPerMeshlet;
    float3     meshletColor  = kMeshletColors[meshletIdx % 16u];

    // Flat-shade with simple NdotL so depth perception is preserved.
    float3 N     = normalize(input.worldN);
    float  NdotL = saturate(dot(N, normalize(float3(0.5f, 1.0f, 0.5f))));
    float3 lit   = meshletColor * (0.35f + 0.65f * NdotL);

    PSOutput o;
    o.color    = float4(lit, 1.0f);
    o.albedo   = float4(meshletColor, 1.0f);
    o.normal   = float4(N * 0.5f + 0.5f, 1.0f);
    o.material = float4(0.5f, 0.0f, 1.0f, 1.0f);
    o.emissive = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}
