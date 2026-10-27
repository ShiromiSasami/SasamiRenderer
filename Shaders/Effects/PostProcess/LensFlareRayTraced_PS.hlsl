// LensFlareRayTraced_PS.hlsl -- shades the ghost mesh traced by LensFlareRayTraced_VS.hlsl.
//
// Follows CAPCOM Open Conference Professional 2023, "Ray Tracing Lens Flare" (RE Engine team):
// a ghost's shape is the aperture stop as seen through the rest of the lens, so this pass only
// needs to clip the mesh to the diaphragm's polygon and shade it with the intensity the trace
// already computed -- no shape or placement logic lives here, unlike LensFlare_PS.hlsl's other
// two modes, both of which build a ghost's shape/position directly in the pixel shader.
//
// The aperture-plane coordinate arrives already normalised by the stop's physical radius (see the
// VS), so this file never needs the lens table's numbers duplicated into it -- the VS's array
// stays the single source of truth for the lens design.

cbuffer LensFlareCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_flareParams;         // xy: light position in UV space, z: intensity, w: aspect ratio (w/h)
    float4 u_flareTint;           // rgb: light colour, w: aperture blade count
    float4 u_flareMode;           // x: mode, y: grid resolution N (vertices per side), z: ghost path count
};

// Regular polygon signed distance, after Inigo Quilez's 2D distance function collection
// (https://iquilezles.org/articles/distfunctions2d/) -- the same shape LensFlare_PS.hlsl's
// procedural mode uses for its diaphragm, reused here rather than shared via #include (this
// project's shaders do not use includes) so the two files stay independently compilable. Keep
// them in sync if the polygon shape is ever revisited. Negative inside.
float SdRegularPolygon(float2 p, float radius, float bladeCount)
{
    const float an = 3.141592653589793f / bladeCount;
    const float2 acs = float2(cos(an), sin(an));

    float bn = fmod(atan2(p.x, p.y) + 6.283185307179586f, 2.0f * an) - an;
    p = length(p) * float2(cos(bn), abs(sin(bn)));
    p -= radius * acs;
    p.y += clamp(-p.y, 0.0f, radius * acs.y);
    return length(p) * sign(p.x);
}

float4 PSMain(float4 position : SV_POSITION, float2 aperturePosition : TEXCOORD0, float intensity : TEXCOORD1,
              float2 gridCoord : TEXCOORD2) : SV_TARGET
{
    const float bladeCount = max(u_flareTint.w, 3.0f);

    // aperturePosition is already normalised to the stop's own radius, so the shape below is
    // tested against a canonical disc rather than the lens table's physical aperture radius.
    // Slightly under 1.0 so the polygon's corners, which extend past its inscribed radius, do not
    // get clipped by the normalisation.
    const float apertureShapeRadius = 0.72f;
    const float edgeSoftness = 0.05f;

    const float d = SdRegularPolygon(aperturePosition, apertureShapeRadius, bladeCount);
    if (d > edgeSoftness)
    {
        discard;
    }

    // Soft edge in the same normalised units as the polygon test -- there is no texel to match
    // since this shape comes from the trace, not a texture.
    const float edge = saturate(1.0f - smoothstep(-edgeSoftness, edgeSoftness, d));

    // Ghosts are double reflections off anti-reflection-coated glass -- typically well under 1%
    // per surface, and a ghost is two of those multiplied together -- so `intensity` here is
    // already small on its own (see the VS's FresnelSchlick calls). Unlike LensFlare_PS.hlsl's
    // procedural mode, there is no separate hand-tuned brightness constant layered on top: the
    // number comes from the trace itself.
    //
    // Simplification: a full implementation would additionally weight each mesh quad by the
    // ratio of its undistorted to distorted area (the Jacobian of the grid mapping) so ghosts
    // conserve irradiance as the talk's method implies. That needs per-quad, not per-vertex,
    // information and is left out here; brightness is the Fresnel product alone.
    // Display scale. The Fresnel product above is physically right and physically invisible: two
    // reflections off coated glass are well under a percent each, so the honest value lands far
    // below the display range and the ghosts render as nothing (verified on screen -- the first
    // build was correct and blank). The other two modes carry the same kind of constant; the
    // difference is that here the *relative* brightness between ghosts is derived from the trace
    // rather than authored, which is the part worth comparing.
    // Energy conservation across the traced grid. The Jacobian of grid-to-screen tells how much
    // entrance-pupil area each pixel covers: large where the trace compressed the grid, small where
    // it fanned out. Multiplying by it makes a spread ghost dimmer per pixel by exactly the factor
    // its area grew, which is what stops the compressed ghosts near the light from piling up into a
    // white blob under additive blending. Hardware derivatives make this free -- the alternative
    // was tracing two extra rays per vertex.
    const float2 dGridDx = ddx(gridCoord);
    const float2 dGridDy = ddy(gridCoord);
    const float jacobian = abs(dGridDx.x * dGridDy.y - dGridDx.y * dGridDy.x);

    // Exposure. One global constant remains, and it is honestly that: the Fresnel product is a
    // physical fraction of a percent squared, far below the display range. What the Jacobian buys
    // is that the *relative* brightness across and between ghosts is now derived rather than tuned.
    const float kDisplayScale = 2.2e5f;
    const float3 colour = u_flareTint.rgb * intensity * jacobian * kDisplayScale * edge * max(u_flareParams.z, 0.0f);

    // Premultiplied alpha: reads as additive without a separate blend-mode switch.
    return float4(colour, saturate(edge * intensity * jacobian * kDisplayScale));
}
