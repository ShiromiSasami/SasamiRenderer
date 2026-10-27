#pragma once

// Shared split-sum specular IBL used by DeferredLighting_PS.hlsl and CookTorranceGGX_PS.hlsl.
// Requires the includer to declare IblPrefilterTex, IblBrdfLutTex, LinearWrap and u_iblParams
// (x: enabled, y: intensity, z: max prefilter mip) before including this header.

// Multiple-scattering energy compensation (Turquin 2019, "Practical multiple scattering
// compensation for microfacet models"; Filament's energyCompensation): the LUT's a+b is the
// single-scatter directional albedo E(NdotV, roughness), and 1 + F0*(1/E - 1) restores the
// energy single-scatter GGX loses at high roughness. Shared by the IBL and the direct-light
// specular so both lobes get the same correction. Returns 1 when IBL is off, because the LUT is
// only loaded together with the rest of the IBL resources.
float3 SpecularEnergyCompensation(float3 F0, float NdotV, float roughness)
{
    if (u_iblParams.x <= 0.5) { return (float3)1; }
    const float2 ab = IblBrdfLutTex.SampleLevel(LinearWrap, saturate(float2(NdotV, roughness)), 0).rg;
    return 1.0 + F0 * (1.0 / max(ab.x + ab.y, 1e-4) - 1.0);
}

// Split-sum specular IBL (Karis 2013, "Real Shading in Unreal Engine 4"):
// the prefiltered radiance cube supplies the pre-integrated lighting term and the
// 2D LUT supplies the BRDF term, so a mirror-like environment response is possible
// where the L2 SH probes used for diffuse GI cannot represent one.
float3 EvaluateSpecularIbl(float3 N, float3 V, float3 F0, float roughness, float specularOcclusion)
{
    if (u_iblParams.x <= 0.5) { return (float3)0; }
    const float NdotV = saturate(dot(N, V));
    const float3 R = reflect(-V, N);
    const float mip = saturate(roughness) * max(u_iblParams.z, 0.0);
    const float3 prefiltered = IblPrefilterTex.SampleLevel(LinearWrap, R, mip).rgb;
    const float2 ab = IblBrdfLutTex.SampleLevel(LinearWrap, saturate(float2(NdotV, roughness)), 0).rg;
    const float3 energyCompensation = SpecularEnergyCompensation(F0, NdotV, roughness);
    return prefiltered * (F0 * ab.x + ab.y) * energyCompensation * specularOcclusion * u_iblParams.y;
}
