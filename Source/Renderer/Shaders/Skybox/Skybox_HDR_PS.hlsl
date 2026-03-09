TextureCube SkyboxTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 dir      : TEXCOORD0;
};

float4 PSMain(PSInput i) : SV_TARGET
{
    // HDR skybox path:
    // input cubemap is expected to be linear HDR (float) radiance.
    float3 sampleDir = normalize(i.dir);
    float3 color = SkyboxTex.Sample(LinearWrap, sampleDir).rgb;

    // Tone map: c' = c / (1 + c)
    color = color / (color + 1.0);

    // Gamma encode (approx sRGB): c_out = c'^(1/2.2)
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
