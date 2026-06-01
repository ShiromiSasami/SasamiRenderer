// ShadowVSM_PS.hlsl
// Writes (depth, depth²) moments for Variance Shadow Maps.
// Depth comes from SV_POSITION.z which is the clip-space depth in [0,1].

struct PSInput
{
    float4 pos : SV_POSITION;
};

float2 PS_ShadowVSM(PSInput i) : SV_TARGET
{
    float d = i.pos.z;
    return float2(d, d * d);
}
