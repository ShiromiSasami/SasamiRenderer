#ifndef SASAMI_PUNCTUAL_LIGHT_FALLOFF_HLSLI
#define SASAMI_PUNCTUAL_LIGHT_FALLOFF_HLSLI

// Distance attenuation for point and spot lights, shared by every lighting path
// (raster forward/deferred, BasicShader, DXR, SWRT reflection/ReSTIR, GI probes) so
// the same light falls off identically everywhere. Selected by
// RenderSettings::punctualLightFalloff (RendererEnums::PunctualLightFalloff).
//
//   0 LinearSquared   saturate(1 - d/r)^2          -- legacy raster/DXR/Basic curve
//   1 SmoothWindow    saturate(1 - (d/r)^2)^2      -- legacy SWRT/GI curve
//   2 InverseSquare   saturate(1 - (d/r)^4)^2 / d^2 -- physical (default)
//
// Mode 2 is the windowed inverse-square law from Karis 2013, "Real Shading in Unreal
// Engine 4" (eq. 9), as also used by Filament (getSquareFalloffAttenuation divided by
// max(d^2, 1e-4)). The window only forces the curve to reach zero at the light's range;
// inside it, intensity follows 1/d^2. The 1 cm floor (1e-4 m^2) keeps a light that
// sits on a surface finite, matching Filament's clamp.
#define PUNCTUAL_FALLOFF_LINEAR_SQUARED 0u
#define PUNCTUAL_FALLOFF_SMOOTH_WINDOW  1u
#define PUNCTUAL_FALLOFF_INVERSE_SQUARE 2u

float PunctualLightFalloff(float dist, float range, uint mode)
{
    const float r = max(range, 1e-4);
    const float t = dist / r;
    if (mode == PUNCTUAL_FALLOFF_LINEAR_SQUARED)
    {
        const float w = saturate(1.0 - t);
        return w * w;
    }
    if (mode == PUNCTUAL_FALLOFF_SMOOTH_WINDOW)
    {
        const float w = saturate(1.0 - t * t);
        return w * w;
    }
    const float t2 = t * t;
    const float w = saturate(1.0 - t2 * t2);
    return (w * w) / max(dist * dist, 1e-4);
}

#endif // SASAMI_PUNCTUAL_LIGHT_FALLOFF_HLSLI
