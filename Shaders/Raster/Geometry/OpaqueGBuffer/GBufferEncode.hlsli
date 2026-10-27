#ifndef SASAMI_GBUFFER_ENCODE_HLSLI
#define SASAMI_GBUFFER_ENCODE_HLSLI

#include "Raster/Lighting/PBR/PBR_BRDF.hlsli"

struct GBufferOutput
{
    float4 albedo           : SV_TARGET0;
    float4 normal           : SV_TARGET1;
    float4 material         : SV_TARGET2;
    float4 emissive         : SV_TARGET3;
    float4 specularWorkflow : SV_TARGET4;
};

// Mirrors the u_material* float4s of the per-draw CameraCB (same order).
struct GBufferMaterial
{
    float4 baseColor;          // u_materialBaseColor
    float4 emissiveRoughness;  // u_materialEmissiveRoughness
    float4 params;             // u_materialParams
    float4 specularWorkflow;   // u_materialSpecularWorkflow
    float4 reflectionParams;   // u_materialReflectionParams
    float  alphaCutoff;        // u_materialTransparencyParams.z; glTF MASK cutoff, 0 = no test
};

struct GBufferSurface
{
    float3 worldN;
    float3 worldPos;
    float2 uv;
    float4 color;
    bool   isFrontFace;
    float3 cameraPos;
};

// Perturbs `n` (unit, world space) with a tangent-space normal map sample.
// Vertices carry no tangents, so the TBN is rebuilt from screen-space
// derivatives of position and UV (Schuler, "Normal Mapping Without
// Precomputed Tangents"). Returns `n` unchanged for a degenerate UV frame.
float3 PerturbNormal(float3 n, float3 worldPos, float2 uv, Texture2D normalTex, SamplerState samp)
{
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, n);
    float3 dp1perp = cross(n, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    // The guard below only exists to avoid dividing by zero on a genuinely degenerate UV
    // frame (duv all zero, or T and B parallel). It must be scale-relative: T is the product
    // of a world-space derivative and a UV derivative, so dot(T,T) scales with the fourth
    // power of the pixel footprint. Against a fixed absolute epsilon it silently drops below
    // the bound as the camera approaches -- at roughly half a metre dot(T,T) lands near
    // 1e-14, so a 1e-12 cut switched normal mapping OFF on close-up surfaces and left a hard
    // seam where the threshold was crossed. Comparing against the same order of magnitude
    // keeps the test dimensionless, so only real degeneracy trips it.
    float maxLen   = max(dot(T, T), dot(B, B));
    float dpScale  = max(dot(dp1, dp1), dot(dp2, dp2));
    float duvScale = max(dot(duv1, duv1), dot(duv2, duv2));
    if (maxLen <= 1e-8 * dpScale * duvScale) {
        return n;
    }
    float invMax = rsqrt(maxLen);

    float3 tsN;
    tsN.xy = normalTex.Sample(samp, uv).xy * 2.0 - 1.0;
    // Reconstruct Z instead of trusting the blue channel: BC5 normal maps carry
    // only two channels and the loader fills blue with a constant.
    tsN.z = sqrt(saturate(1.0 - dot(tsN.xy, tsN.xy)));

    return normalize(T * (invMax * tsN.x) + B * (invMax * tsN.y) + n * tsN.z);
}

// Geometric specular antialiasing (Tokuyoshi & Kaplanyan 2019, "Improved Geometric Specular
// Antialiasing", I3D; same form as Filament's specularAntiAliasing). Screen-space normal variation
// is folded into GGX alpha^2 so sub-pixel normal detail widens the lobe instead of aliasing into
// sparkles. Takes and returns perceptual roughness (alpha = roughness^2).
static const float SPECULAR_AA_VARIANCE = 0.15;  // screen-space filter kernel variance (Filament default)
static const float SPECULAR_AA_THRESHOLD = 0.2;  // clamp on the added alpha^2 (Filament default)

float SpecularAntiAliasRoughness(float3 N, float roughness)
{
    float3 du = ddx(N);
    float3 dv = ddy(N);
    float variance = SPECULAR_AA_VARIANCE * (dot(du, du) + dot(dv, dv));
    float kernelRoughness2 = min(2.0 * variance, SPECULAR_AA_THRESHOLD);
    float alpha = roughness * roughness;
    float filteredAlpha2 = saturate(alpha * alpha + kernelRoughness2);
    return sqrt(sqrt(filteredAlpha2));
}

GBufferOutput EncodeGBuffer(GBufferSurface s, GBufferMaterial m,
                            Texture2D albedoTex, Texture2D occlusionTex, Texture2D normalTex,
                            Texture2D emissiveTex, SamplerState samp)
{
    float4 albedoSample = albedoTex.Sample(samp, s.uv);
    float3 albedo = albedoSample.rgb * s.color.rgb * m.baseColor.rgb;
    float materialAlpha = saturate(albedoSample.a * s.color.a * m.baseColor.a);
    // glTF 2.0 3.9.6: MASK renders the texel fully opaque or discards it.
    if (m.alphaCutoff > 0.0)
    {
        clip(materialAlpha - m.alphaCutoff);
    }

    const float4 materialTextureSample = occlusionTex.Sample(samp, s.uv);
    const float aoSample = saturate(materialTextureSample.r);
    const float occlusionStrength = saturate(m.params.y);
    float materialAo = lerp(1.0, aoSample, occlusionStrength);

    const bool useSpecularGlossiness = IsSpecularGlossinessWorkflow(m.specularWorkflow.w);
    float metallic = useSpecularGlossiness ? 0.0 : saturate(m.params.x);
    float roughness = saturate(m.emissiveRoughness.w);
    if (!useSpecularGlossiness && m.params.w > 0.5) {
        roughness = saturate(roughness * materialTextureSample.g);
        metallic = saturate(metallic * materialTextureSample.b);
    }

    float3 geometricN = normalize(s.worldN);
    float3 N = PerturbNormal(geometricN, s.worldPos, s.uv, normalTex, samp);

    // Two-sided shading for every back face, not just materials that opted in.
    //
    // The rasterizer runs with D3D12_CULL_MODE_NONE (RenderPipelineStateCache.cpp), so a
    // surface's back side is always rasterized. Its interpolated normal still points out
    // the front, away from the viewer, which made the interior of walls light up from a
    // sun that is outside and occluded -- the light appeared to pass through the mesh.
    // Whichever side is actually being shaded, the shading normal must face the viewer.
    //
    // This used to be gated on SurfaceMaterial::doubleSided to avoid touching meshes with
    // questionable winding/normals (Bistro FBX). That gate is what left the symptom in
    // place: Bistro never sets the flag, so its back faces kept the outward normal. If a
    // mesh does turn out to have inverted winding, the fix belongs in the asset or in
    // per-material culling, not in leaving back faces shaded from the wrong side.
    if (!s.isFrontFace) {
        N = -N;
        geometricN = -geometricN;
    }
    roughness = SpecularAntiAliasRoughness(N, roughness);
    float3 F0 = useSpecularGlossiness
        ? saturate(m.specularWorkflow.rgb)
        : lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float authoredReflectionMask = ReflectionMaterialMask(m.reflectionParams.x);
    float physicalReflectionMask = useSpecularGlossiness
        ? saturate(max(max(F0.r, F0.g), F0.b) * (1.0 - roughness))
        : saturate(metallic * (1.0 - roughness));

    GBufferOutput o;
    o.albedo   = float4(saturate(albedo), materialAlpha);
    o.normal   = float4(N * 0.5 + 0.5, length(s.worldPos - s.cameraPos));
    o.material = float4(roughness, metallic, materialAo, max(authoredReflectionMask, physicalReflectionMask));
    // Emissive alpha carries the horizon *occlusion* (1 - HorizonOcclusion) for the deferred
    // pass. HorizonOcclusion needs the vertex (geometric) normal, which the GBuffer does not
    // otherwise store -- the normal target holds the normal-mapped shading normal, and feeding
    // that to the horizon test made it a no-op (dot(R, N) = NdotV >= 0 always). R and V are
    // both known here, so the scalar result fits in a channel that was unused, instead of a new
    // render target. Stored inverted so writers that leave alpha at 0 mean "not occluded".
    float3 V = normalize(s.cameraPos - s.worldPos);
    float horizonOcclusion = 1.0 - HorizonOcclusion(reflect(-V, N), geometricN);
    // glTF 2.0 3.9.3: emissive = emissiveFactor * emissiveTexture (sRGB view, so already linear).
    // A material without one binds a white fallback, which keeps emissive = factor.
    const float3 emissive = m.emissiveRoughness.rgb * emissiveTex.Sample(samp, s.uv).rgb;
    o.emissive = float4(emissive, horizonOcclusion);
    // Forward the selector unchanged so the lighting pass can tell Toon from the two PBR
    // workflows; collapsing it to 0/1 here would erase the Toon bucket.
    o.specularWorkflow = float4(saturate(m.specularWorkflow.rgb), m.specularWorkflow.w);
    return o;
}

#endif // SASAMI_GBUFFER_ENCODE_HLSLI
