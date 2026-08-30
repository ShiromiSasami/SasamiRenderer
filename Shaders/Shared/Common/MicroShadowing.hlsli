#ifndef SASAMI_MICRO_SHADOWING_HLSLI
#define SASAMI_MICRO_SHADOWING_HLSLI

// Chan 2018, "Material Advances in Call of Duty: WWII" (adopted by Filament as
// computeMicroShadowing). AO defines the aperture of an unoccluded visibility
// cone; light directions grazing that cone are attenuated. Applied on top of
// shadow-map visibility — it models small-scale occlusion shadow maps miss.
// strength lerps the effect in ([0,1], 0 = off).
float ComputeMicroShadowing(float NdotL, float ao, float strength)
{
    float aperture = rsqrt(1.0 - min(ao, 0.9999));
    float microShadow = saturate(NdotL * aperture);
    microShadow *= microShadow;
    return lerp(1.0, microShadow, saturate(strength));
}

#endif // SASAMI_MICRO_SHADOWING_HLSLI
