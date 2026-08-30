#ifndef SASAMI_WORLD_POS_RECONSTRUCTION_HLSLI
#define SASAMI_WORLD_POS_RECONSTRUCTION_HLSLI

// Reconstructs world-space position from a UV + device depth using the
// inverse view-projection matrix. Callers must declare a `row_major float4x4
// u_cameraInvPV;` cbuffer field before including this header.
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f,
                        (1.0f - uv.y) * 2.0f - 1.0f,
                        depth,
                        1.0f);
    float4 worldH = mul(ndc, u_cameraInvPV);
    return worldH.xyz / max(worldH.w, 1e-6f);
}

#endif // SASAMI_WORLD_POS_RECONSTRUCTION_HLSLI
