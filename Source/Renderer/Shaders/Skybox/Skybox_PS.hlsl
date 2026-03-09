TextureCube SkyboxTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 dir      : TEXCOORD0;
};

float4 PSMain(PSInput i) : SV_TARGET
{
    // LDR skybox path:
    // source cubemap is expected to be display-domain LDR color.
    float3 sampleDir = normalize(i.dir);
    float3 color = SkyboxTex.Sample(LinearWrap, sampleDir).rgb;
    return float4(color, 1.0);
}
