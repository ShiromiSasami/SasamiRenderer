// Wireframe_PS.hlsl
// Pixel shader for the editor wireframe overlay: a flat, unlit line colour.

float4 PSMain() : SV_TARGET
{
    return float4(0.35, 0.85, 1.0, 1.0);
}
