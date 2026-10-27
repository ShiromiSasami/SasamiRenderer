// ToonOutline_PS.hlsl
// Pixel shader for the toon inverted-hull outline pass. Outputs a flat, unlit
// colour for the outline -- no shading, just the configured line colour.

#include "Shared/Common/LightCB.hlsli"

float4 PSMain() : SV_TARGET
{
    return float4(u_toonOutline.yzw, 1.0);
}
