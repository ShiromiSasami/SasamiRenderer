struct PSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float2 centered = input.uv * 2.0 - 1.0;
    float d = length(centered);
    float alpha = saturate(1.0 - d);
    alpha = alpha * alpha;
    alpha *= input.color.a;
    if (alpha < 0.01) discard;
    return float4(input.color.rgb, alpha);
}
