struct MeshPushConstants
{
    row_major float4x4 modelViewProjection;
    float4 baseColor;
    float4 lightDirIntensity;
    float4 lightColor;
    float4 emissiveRoughness;
};

[[vk::push_constant]]
ConstantBuffer<MeshPushConstants> g_mesh : register(b0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), g_mesh.modelViewProjection);
    output.normal = normalize(input.normal);
    output.color = input.color * g_mesh.baseColor;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normal);
    float3 l = normalize(-g_mesh.lightDirIntensity.xyz);
    float ndotl = saturate(dot(n, l));
    float ambient = 0.16f;
    float3 lit = input.color.rgb * g_mesh.lightColor.rgb * (ambient + ndotl * g_mesh.lightDirIntensity.w);
    lit += g_mesh.emissiveRoughness.rgb;
    lit = lit / (lit + 1.0f);
    lit = pow(saturate(lit), 1.0f / 2.2f);
    return float4(lit, input.color.a);
}
