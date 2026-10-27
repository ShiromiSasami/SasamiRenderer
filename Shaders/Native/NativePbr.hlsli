#pragma once

// Cook-Torrance microfacet BRDF (GGX distribution + height-correlated Smith visibility +
// Schlick Fresnel) following Karis, "Real Shading in Unreal Engine 4",
// SIGGRAPH 2013 "Physically Based Shading in Theory and Practice" course notes.

static const float kPi = 3.14159265359;

// Trowbridge-Reitz / GGX normal distribution. alpha = roughness^2 per Karis's remapping so that
// perceptual roughness stays roughly linear.
float D_GGX(float NoH, float roughness)
{
    // Floor alpha (Filament MIN_PERCEPTUAL_ROUGHNESS 0.045) rather than the denominator, which
    // flattened the peak below roughness ~0.24.
    float alpha = max(roughness * roughness, 0.002025);
    float alpha2 = alpha * alpha;
    float denom = (NoH * NoH) * (alpha2 - 1.0) + 1.0;
    denom = kPi * denom * denom;
    return alpha2 / denom;
}

// Height-correlated Smith visibility term, already folded together with the 1/(4 NoV NoL)
// denominator of the specular BRDF so the caller multiplies D * V * F directly.
float V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float lambdaV = NoL * sqrt(max(NoV * NoV * (1.0 - alpha2) + alpha2, 1e-5));
    float lambdaL = NoV * sqrt(max(NoL * NoL * (1.0 - alpha2) + alpha2, 1e-5));
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Schlick's approximation of the Fresnel reflectance at grazing angle VoH.
float3 F_Schlick(float3 f0, float VoH)
{
    float f = pow(saturate(1.0 - VoH), 5.0);
    return f0 + (1.0 - f0) * f;
}

// Combines the specular (GGX) and diffuse (Lambert) terms into outgoing radiance for a single
// directional light. Dielectrics get a fixed f0 of 0.04 (~4% reflectance), metals tint f0 with
// albedo, and the diffuse term is energy-compensated by (1 - metallic) as metals have no
// sub-surface diffuse response.
float3 EvaluatePbr(float3 albedo, float metallic, float roughness, float3 n, float3 v, float3 l,
                    float3 lightColor, float lightIntensity)
{
    float3 h = normalize(v + l);
    float NoV = max(dot(n, v), 1e-5);
    float NoL = saturate(dot(n, l));
    float NoH = saturate(dot(n, h));
    float VoH = saturate(dot(v, h));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float D = D_GGX(NoH, roughness);
    float V = V_SmithGGXCorrelated(NoV, NoL, roughness);
    float3 F = F_Schlick(f0, VoH);
    float3 specular = D * V * F;

    float3 diffuse = albedo * (1.0 - metallic) / kPi;

    return (diffuse + specular) * lightColor * lightIntensity * NoL;
}
