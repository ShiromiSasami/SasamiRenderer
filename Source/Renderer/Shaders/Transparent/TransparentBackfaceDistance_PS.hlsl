#include "../Common/LightCB.hlsli"

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

float PSMain(PSInput i) : SV_TARGET0
{
    return length(i.worldPos - u_cameraPos.xyz);
}
