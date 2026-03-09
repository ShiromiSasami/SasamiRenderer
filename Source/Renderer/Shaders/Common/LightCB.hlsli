#ifndef SASAMI_LIGHT_CB_LAYOUT_INCLUDED
#define SASAMI_LIGHT_CB_LAYOUT_INCLUDED

#define DIFFUSE_SH_COEFFICIENT_COUNT 9

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP;
    float4 u_dirDir;      // xyz: forward dir, w: intensity
    float4 u_dirColor;    // rgb: color
    float4 u_lightCounts; // x: pointCount, y: spotCount
    float4 u_cameraPos;   // xyz: camera world position
    float4 u_iblParams;   // x: IBL enable, y: intensity, z: prefilter max mip, w: diffuse SH enable
    float4 u_debugParams; // x: gbuffer debug view mode
    float4 u_diffuseSh[DIFFUSE_SH_COEFFICIENT_COUNT];
}

#endif
