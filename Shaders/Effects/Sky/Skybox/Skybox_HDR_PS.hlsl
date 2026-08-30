#include "Effects/Sky/Skybox/Skybox_Common.hlsli"

TextureCube SkyboxTex : register(t0);
SamplerState LinearWrap : register(s0);

float4 PSMain(PSInput i) : SV_TARGET
{
    float3 sampleDir = normalize(i.dir);
    float3 color = SkyboxTex.Sample(LinearWrap, sampleDir).rgb;
    color = ApplyDirectionalLightMarker(color, sampleDir, 0.35, 0.5, 6.0, false);

    return float4(color, 1.0);
}
