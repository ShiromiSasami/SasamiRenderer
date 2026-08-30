//
// FluidSurface_PS.hlsl
// V1 water-like shading: Lambertian diffuse against a bluish-teal base color,
// a small constant ambient term, and a Blinn-Phong specular highlight using
// the true view direction (camera position bound via CameraCB.extra0, same
// root CBV as FluidSurface_VS's b0). No reflections/refraction/screen-space
// effects. Outputs to R16G16B16A16Float HDR scene color (no tone mapping here).
//

// Mirrors FluidSurface_VS.hlsl's CameraCB (b0) byte-for-byte; only extra0
// (camera world position, xyz) is used here.
cbuffer CameraCB : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 world;
    float4             extra0; // xyz = camera world position
    float4             extra1;
    float4             extra2;
};

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

float4 PSMain(PSInput input) : SV_Target0
{
    float3 n = normalize(input.worldNormal);

    float3 lightDir     = normalize(float3(0.3f, -0.8f, 0.5f)); // light-to-surface
    float3 toLight       = -lightDir;
    float3 baseColor     = float3(0.05f, 0.25f, 0.35f);
    float3 ambient        = 0.08f * baseColor;

    float ndotl   = max(dot(n, toLight), 0.0f);
    float3 diffuse = baseColor * ndotl;

    float3 viewDir = normalize(extra0.xyz - input.worldPos);
    float3 halfVec = normalize(toLight + viewDir);
    float specularPower = 64.0f;
    float specular = pow(max(dot(n, halfVec), 0.0f), specularPower) * 0.5f;

    float3 color = ambient + diffuse + specular.xxx;
    float alpha = 0.85f;

    return float4(color, alpha);
}
