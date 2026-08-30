#ifndef SASAMI_SWRT_DENOISE_COMMON_HLSLI
#define SASAMI_SWRT_DENOISE_COMMON_HLSLI

// Shared helpers for the SWRT SVGF/A-Trous denoise passes.

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

#endif
