#pragma once

namespace SasamiEngine::Render::Voxel
{
    // Colour helpers for the stylised fire of WindEffectsOverlay (CPU, no GPU state).

    // Blackbody colour of a temperature in kelvin as 8-bit sRGB in [0, 255], Tanner Helland's curve fit
    // (2012, "How to Convert Temperature (K) to RGB"); valid for 1000..40000 K, clamped to that range.
    void BlackbodySrgb(float kelvin, float outRgb[3]);

    // Temperature of a flame puff at normalised age t in [0, 1]: hot at the root (t=0, ~2600 K, yellow-white),
    // cooling towards the tip (t=1, 1000 K, deep red). Real flames are not in thermal equilibrium; this is
    // only the usual artistic ramp (white/yellow core -> orange -> red, as in most real-time fire shaders).
    float FlameTemperature(float age01);

    // Smooth 2D value noise in [0, 1] (bilinear with a smoothstep fade, integer hash lattice).
    float ValueNoise2(float x, float y);

    // Two octaves of ValueNoise2 remapped to [-1, 1].
    float FlameNoise(float x, float y);
}
