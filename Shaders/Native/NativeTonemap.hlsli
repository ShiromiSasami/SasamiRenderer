#pragma once

// Tonemap shared by the DX11 and Vulkan native paths, so they match the D3D12 path's curve.
// Reference: Shaders/Effects/PostProcess/ToneMap_PS.hlsl (Narkowicz 2015 ACES fit, applied after exposure).
// Included by relative path (D3D_COMPILE_STANDARD_FILE_INCLUDE resolves against the including file).


float3 NativeAcesFilm(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// exposure = RenderSettings::exposure, fed per backend from RhiBackendMeshFrameDesc::exposure.
float3 NativeTonemap(float3 hdr, float exposure)
{
    return NativeAcesFilm(hdr * exposure);
}
