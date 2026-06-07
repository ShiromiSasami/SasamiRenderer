// Tessellation_Debug_PS.hlsl
// Uses the per-patch debug color authored in the tessellation HS/DS path and
// outputs to the same 5 GBuffer RTVs as CookTorranceGGX_PS.

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

PSOutput PSMain(PSInput input)
{
    float3 patchColor = saturate(input.color.rgb);

    // Simple NdotL with a fixed sun direction to preserve depth perception.
    float3 N     = normalize(input.worldN);
    float  NdotL = saturate(dot(N, normalize(float3(0.5f, 1.0f, 0.5f))));
    float3 lit   = patchColor * (0.35f + 0.65f * NdotL);

    PSOutput o;
    o.color    = float4(lit, 1.0f);
    o.albedo   = float4(patchColor, 1.0f);
    o.normal   = float4(N * 0.5f + 0.5f, 1.0f);
    o.material = float4(0.5f, 0.0f, 1.0f, 1.0f);
    o.emissive = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}
