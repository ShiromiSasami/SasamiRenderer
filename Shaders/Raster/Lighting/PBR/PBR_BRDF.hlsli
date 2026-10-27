#ifndef SASAMI_PBR_BRDF_HLSLI
#define SASAMI_PBR_BRDF_HLSLI

// --- PBR helpers (Cook-Torrance microfacet model) ---
// D_GGX(N,H,alpha) = alpha^2 / (PI * ((N.H)^2 * (alpha^2 - 1) + 1)^2)
// alpha is floored at 0.045^2 (Filament's MIN_PERCEPTUAL_ROUGHNESS for fp32) instead of clamping
// the denominator: d >= a2 > 0 on [0,1], so the floor alone keeps it finite. A denominator clamp
// of 1e-4 flattened the peak of every lobe below roughness ~0.27 (r=0.2: 199 -> 16).
static const float MIN_GGX_ALPHA = 0.002025;

float DistributionGGX(float NdotH, float a)
{
    a = max(a, MIN_GGX_ALPHA);
    float a2 = a * a;
    float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d);
}

// Schlick-GGX geometry approximation:
// G1(v) = (N.V) / ((N.V) * (1 - k) + k)
float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

// Smith masking-shadowing for both view/light:
// G_Smith = G1(N.V) * G1(N.L)
float GeometrySmith(float NdotV, float NdotL, float k)
{
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// Height-correlated Smith visibility, V = G / (4 N.V N.L): the specular BRDF is then D * V * F.
// Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs";
// Karis 2014 / Filament (V_SmithGGXCorrelated). Takes perceptual roughness; alpha uses the same
// floor as DistributionGGX so alpha^2 stays consistent with D.
float V_SmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
    float a = max(roughness * roughness, MIN_GGX_ALPHA);
    float a2 = a * a;
    float lambdaV = NdotL * sqrt(max(NdotV * NdotV * (1.0 - a2) + a2, 1e-5));
    float lambdaL = NdotV * sqrt(max(NdotL * NdotL * (1.0 - a2) + a2, 1e-5));
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Schlick Fresnel approximation:
// F(cosTheta) = F0 + (1 - F0) * (1 - cosTheta)^5
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

// Specular occlusion approximation for image-based specular term.
// One practical form used in real-time renderers:
// SO = saturate((N.V + AO)^(2^(-16*roughness - 1)) - 1 + AO)
// How much of this pixel's indirect specular the reflection composite pass will supply.
//
// The composite (SWRT_ReflectionComposite_PS.hlsl / the SSR composite share one PSO) adds its
// contribution on top of SceneColor with an additive blend. The lighting pass had already put a
// full-strength EvaluateSpecularIbl there, so the same indirect specular was counted twice on
// every reflective surface. The lighting pass now fades its IBL specular out by this coverage,
// which makes the total IBL*(1-c) + traced*c: no double count, and misses (alpha 0) still keep
// their IBL specular instead of going black.
//
// Both sides MUST derive the coverage from this one function. If the lighting pass and the
// composite disagree even slightly, the difference shows up as a brightness seam exactly along
// the edge of the traced reflection.
// Mirrors SurfaceMaterial::EncodeShadingSelector (Source/Renderer/Scene/SurfaceMaterial.h).
// The selector arrives either straight from the material CB or via GBufferSpecularWorkflow.a,
// which is R8G8B8A8_UNORM -- so 0.5 comes back as ~0.502 and an equality test would miss it.
// Compare against the midpoints instead.
//   0.0 = Standard / metallic-roughness, 0.5 = Toon, 1.0 = Standard / specular-glossiness
bool IsToonShading(float selector)              { return selector > 0.25 && selector < 0.75; }
// Toon face with SDF face shadow: written by OpaqueGBuffer_PS only (0.375 -> ~0.376 through UNORM8).
// specularWorkflow.r then holds the face-shadow lit factor in place of N.L.
bool IsToonFaceShading(float selector)          { return selector > 0.25 && selector < 0.4375; }
// Toon hair (SurfaceMaterial::toonHair): 0.625 -> ~0.624 through UNORM8.
bool IsToonHairShading(float selector)          { return selector > 0.5625 && selector < 0.75; }
bool IsSpecularGlossinessWorkflow(float selector) { return selector >= 0.75; }

// ---------------------------------------------------------------------------
// Cel shading (ShadingModel::Toon)
//
// Replaces the Cook-Torrance evaluation rather than post-processing it: quantizing a finished
// PBR result still leaves the specular lobe's gradient inside each band, which reads as "PBR
// with posterization" instead of cel shading.
// ---------------------------------------------------------------------------

// Quantizes lambert into `bands` steps. Shadow visibility is folded into the ramp input on
// purpose: quantizing lighting and shadow separately leaves the shadow terminator as a smooth
// PBR gradient next to hard-banded lighting, which looks like a bug rather than a style.
float ToonDiffuseRamp(float NdotL, float visibility, float bands, float softness, float shadowFloor)
{
    const float steps = max(bands, 1.0);
    const float t = saturate(NdotL) * saturate(visibility);
    const float scaled = t * steps;
    const float lower = floor(scaled);
    // Smoothstep across the step edge; without it the band boundary aliases badly on curved
    // surfaces, which is the usual giveaway of a naive floor()-based ramp.
    const float edge = smoothstep(0.0, max(softness * steps, 1e-4), scaled - lower);
    const float ramp = saturate((lower + edge) / steps);
    // Lift the darkest band off zero. A cel shadow is a darker *tone*, not the absence of light;
    // leaving it at 0 makes every surface facing away from the key light read as a black hole,
    // which is what the first version of this looked like.
    return lerp(shadowFloor, 1.0, ramp);
}

// Flat highlight instead of a GGX lobe: on/off across a threshold with a soft edge.
float ToonSpecularTerm(float NdotH, float threshold, float softness)
{
    const float soft = max(softness, 1e-4);
    return smoothstep(threshold - soft, threshold + soft, NdotH);
}

// Toon hair highlight (angel ring). Scheuermann, "Hair Rendering and Shading", GDC 2004:
// Kajiya-Kay strand specular sin(T,H) on a tangent shifted along the normal, one lobe per
// Marschner highlight. The toon version thresholds sin(T,H) instead of raising it to a power.
//
// The GBuffer has no tangent, so the strand direction is the head "up" projected onto the
// shading plane: strands run crown to tip. That is an approximation of ours, not Scheuermann's
// (he reads T from the mesh); it makes the band a ring around the skull, which is the anime look,
// and needs smooth normals -- the same requirement the projected-sphere angel rings have.
// Returns 0 where the normal is parallel to up (the crown), where the strand direction is undefined.
float3 ToonHairStrandTangent(float3 N, float3 up)
{
    const float3 t = up - N * dot(N, up);
    const float len2 = dot(t, t);
    return len2 > 1e-6 ? t * rsqrt(len2) : float3(0.0, 0.0, 0.0);
}

// width: band half-width in 1 - sin(T,H); softness: smoothstep half-width around that edge.
float ToonHairBand(float3 T, float3 N, float3 H, float shift, float width, float softness)
{
    if (dot(T, T) < 0.5) {
        return 0.0;
    }
    const float3 shifted = normalize(T + shift * N);
    const float dotTH = dot(shifted, H);
    const float sinTH = sqrt(saturate(1.0 - dotTH * dotTH));
    const float dirAtten = smoothstep(-1.0, 0.0, dotTH);
    const float edge = 1.0 - width;
    const float soft = max(softness, 1e-4);
    return dirAtten * smoothstep(edge - soft, edge + soft, sinTH);
}

// Fresnel-shaped edge light, the third element that reads as "anime" alongside bands and outline.
//
// Kept as a smooth falloff rather than a thresholded step. An earlier version ran the Fresnel
// through smoothstep around a fixed 0.5: with the default power of 3 that only fires below
// N.V ~= 0.19, a sliver a couple of pixels wide, so the rim was invisible in practice. A hard
// rim band would also duplicate what the outline pass already draws.
float ToonRimTerm(float NdotV, float NdotL, float power)
{
    const float rim = pow(saturate(1.0 - NdotV), max(power, 1e-3));
    // Gated by the key light so the rim wraps the lit silhouette instead of ringing the whole
    // object uniformly, which is how it is normally drawn.
    return rim * smoothstep(0.0, 0.35, NdotL);
}

float ReflectionCoverage(float reflectionAlpha,
                         float reflectionStrength,
                         float materialReflectionMask,
                         float roughness)
{
    const float roughnessVisibility = 1.0 - smoothstep(0.65, 1.0, roughness);
    return saturate(reflectionAlpha * reflectionStrength) * materialReflectionMask * roughnessVisibility;
}

float SpecularOcclusion(float ao, float NdotV, float roughness)
{
    const float exponent = exp2(-16.0 * roughness - 1.0);
    return saturate(pow(saturate(NdotV + ao), exponent) - 1.0 + ao);
}

// Horizon occlusion (Lagarde & de Rousiers, "Moving Frostbite to PBR", SIGGRAPH 2014
// course notes): split-sum specular IBL samples the pre-filtered environment along the
// mirror reflection vector R, but at grazing angles R can dip below the local geometric
// surface even while the shading normal keeps NdotV positive (curved/low-poly geometry,
// bump/normal maps). SpecularOcclusion above has no notion of the geometric surface and
// happily returns full visibility, so the prefiltered cube leaks light through geometry
// that should be occluding it. Fading the response out as dot(R, Ngeo) goes negative
// removes that leak. horizon = saturate(1 + dot(R, Ngeo)), squared to sharpen the falloff.
float HorizonOcclusion(float3 reflectionDir, float3 geometricNormal)
{
    float horizon = saturate(1.0 + dot(reflectionDir, geometricNormal));
    return horizon * horizon;
}

// Shared indirect-specular occlusion term for both the deferred and forward lighting
// passes (DeferredLighting_PS.hlsl / CookTorranceGGX_PS.hlsl). Dielectrics blend toward
// the cheaper ao^2 falloff while metals use the full SpecularOcclusion curve, so a fully
// metallic surface does not lose its whole reflection under mild AO; HorizonOcclusion is
// then applied on top to catch reflection vectors that point below the geometric surface.
// Keeping this in one place means the two passes cannot silently drift apart and disagree
// about how occluded the same surface's specular response is.
float ComputeSpecularOcclusionWithHorizon(float ao, float NdotV, float roughness, float metallic, float horizon)
{
    float so = lerp(ao * ao, saturate(SpecularOcclusion(ao, NdotV, roughness)), saturate(metallic));
    return so * horizon;
}

float ComputeSpecularOcclusion(float ao, float NdotV, float roughness, float metallic, float3 reflectionDir, float3 geometricNormal)
{
    return ComputeSpecularOcclusionWithHorizon(ao, NdotV, roughness, metallic,
                                               HorizonOcclusion(reflectionDir, geometricNormal));
}

float ReflectionMaterialMask(float reflectionStrength)
{
    return saturate(reflectionStrength);
}

#endif // SASAMI_PBR_BRDF_HLSLI
