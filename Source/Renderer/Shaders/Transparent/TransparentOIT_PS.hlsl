#define PSMain CookTorranceGGX_EvaluateForOIT
#include "../CookTorranceGGX_PS.hlsl"
#undef PSMain

struct OITOutput
{
    float4 accum : SV_TARGET0;
    float4 revealage : SV_TARGET1;
};

OITOutput PSMain(PSInput input)
{
    PSOutput lit = CookTorranceGGX_EvaluateForOIT(input);
    float alpha = saturate(lit.color.a);
    float4 albedoSample = AlbedoTex.Sample(LinearWrap, input.uv);
    float3 baseColor = saturate(albedoSample.rgb * input.color.rgb * u_materialBaseColor.rgb);
    float transmission = saturate(u_materialReflectionParams.y);
    float shellStrength = max(u_materialTransparencyParams.x, 0.0);
    float metallic = saturate(u_materialParams.x);
    float3 Vvec = u_cameraPos.xyz - input.worldPos;
    float vLen = length(Vvec);
    float3 V = (vLen > 1e-5) ? (Vvec / vLen) : float3(0.0, 0.0, 1.0);
    float3 N = normalize(input.worldN);
    float nDotV = saturate(dot(N, V));
    float fresnelEdge = pow(1.0 - nDotV, 5.0);

    // Alpha blend and transmission are separate controls. The lit BSDF can be
    // very dark when no reflection/IBL source is available, which makes
    // alpha-blended glass disappear. Preserve a visible surface tint for
    // coverage alpha, and keep a Fresnel shell for transmissive glass.
    float alphaBlendSurface = 1.0 - transmission * 0.45;
    float transmissionShell = transmission * shellStrength * (0.22 + 0.42 * fresnelEdge);
    float surfaceVisibility = saturate(max(alphaBlendSurface, transmissionShell) * (1.0 - metallic));
    float3 shellTint = baseColor * transmission * shellStrength * (0.08 + 0.35 * fresnelEdge) * (1.0 - metallic);
    float3 transparentRadiance = max(lit.color.rgb, baseColor * surfaceVisibility + shellTint);

    // Coverage alpha controls revealage in weighted blended OIT. For
    // transmissive surfaces, keep a small Fresnel-weighted shell so low-alpha
    // glass still has a readable silhouette without making face-on regions
    // opaque.
    float shellAlpha = transmission * shellStrength * (0.04 + 0.28 * fresnelEdge) * (1.0 - metallic);
    float coverageAlpha = saturate(max(alpha, shellAlpha));

    // Weighted blended OIT: accumulate premultiplied transparent radiance and
    // multiplicative revealage. This is order independent, but still an
    // approximation for thick/nested media.
    float z = saturate(input.position.z);
    float weight = clamp(pow(min(1.0, coverageAlpha * 10.0) + 0.01, 3.0) *
                         100000000.0 *
                         pow(1.0 - z * 0.9, 3.0),
                         0.01,
                         3000.0);

    OITOutput o;
    o.accum = float4(transparentRadiance * coverageAlpha * weight, coverageAlpha * weight);
    o.revealage = float4(coverageAlpha, coverageAlpha, coverageAlpha, coverageAlpha);
    return o;
}
