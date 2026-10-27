// DirectX 11 native depth+normal prepass pixel shader.
//
// Writes the world-space normal in the exact encoding SSAO_PS.hlsl expects to find in the D3D12
// GBuffer normal attachment: SSAO_NormalDecode.hlsli decodes with `encoded * 2 - 1`, so the
// matching encode is `n * 0.5 + 0.5`. The target format is R16G16B16A16_FLOAT, the same format
// RenderTargetPool gives GBufferNormal, so the shared decode stays bit-for-bit applicable.
//
// Despite the "view-space" wording in SSAO_NormalDecode.hlsli's header comment, SSAO_PS.hlsl uses
// the decoded normal against a world-space camera vector and projects its samples with the
// world-to-clip matrix, so the value it wants -- and the value written here -- is world space.
#include "NativeDepthNormalCommon.hlsli"

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.worldN);
    return float4(n * 0.5 + 0.5, 1.0);
}
