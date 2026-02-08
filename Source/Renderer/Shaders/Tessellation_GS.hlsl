struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

[maxvertexcount(3)]
void GSMain(triangle PSInput input[3], inout TriangleStream<PSInput> triStream)
{
    // Pass-through geometry shader
    triStream.Append(input[0]);
    triStream.Append(input[1]);
    triStream.Append(input[2]);
    triStream.RestartStrip();
}
