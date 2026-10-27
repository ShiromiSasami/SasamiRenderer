//
// FluidSurface_PS.hlsl
// V1 water-like shading: Lambertian diffuse against a bluish-teal base color,
// a small constant ambient term, and a Blinn-Phong specular highlight using
// the true view direction (camera position bound via CameraCB.extra0, same
// root CBV as FluidSurface_VS's b0). No reflections/refraction/screen-space
// effects. Outputs to R16G16B16A16Float HDR scene color (no tone mapping here).
//

// The renderer's own analytic sky, shared with ProceduralSky_PS and the SWRT miss path. Including
// it rather than binding a cubemap means the water reflects exactly the sky the renderer draws, and
// costs no root binding -- the function needs only a direction and the sun, both already here.
#include "Effects/Sky/ProceduralSky/ProceduralSky.hlsli"

// Mirrors FluidSurface_VS.hlsl's CameraCB (b0) byte-for-byte; only extra0
// (camera world position, xyz) is used here.
cbuffer CameraCB : register(b0)
{
    row_major float4x4 viewProj;
    row_major float4x4 world;
    float4             extra0; // xyz = camera world position
    float4             extra1;
    float4             extra2;
    float4             extra3; // x: backdrop bound (0/1), y: refraction strength
};

// The scene behind the water, snapshotted by FluidBackdropCopyRenderPass immediately before this
// pass. A pass cannot read its own render target, which is why this is a copy rather than SceneColor.
Texture2D    BackdropTex : register(t1);
SamplerState BackdropSampler : register(s0);

struct PSInput
{
    float4 svPosition : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float  agitation   : TEXCOORD2;   // surface disturbance from FluidSurface_VS; drives foam
};

// Foam appears past a threshold rather than proportionally: calm water carries none at all, and
// fading it in from zero everywhere just greys the whole surface. The same thresholded-whitecap
// idea drives ocean rendering (the Jacobian test in Tessendorf-style FFT water) and particle
// fluids alike -- Ihmsen et al., "Unified Spray, Foam and Air Bubbles for Particle-Based Fluids"
// (The Visual Computer, 2012), generate foam only where a trapped-air criterion is exceeded.
static const float kFoamThreshold = 0.55f;
static const float kFoamRange     = 1.30f;
// Spray is the bright tip of the same signal: where the surface is violently disturbed, the
// whitecap blows out rather than staying matte. Actual airborne droplets need real particles
// (that is a separate item) -- this is the surface-shading half.
static const float kSprayThreshold = 1.60f;

float4 PSMain(PSInput input) : SV_Target0
{
    float3 n = normalize(input.worldNormal);

    // extra1.xyz is the scene's directional light (light-to-surface), extra1.w its intensity and
    // extra2.rgb its colour, filled by FluidSurfaceRenderPass. This used to be a hardcoded vector,
    // so the water was lit from a direction the rest of the scene never used.
    float3 lightDir     = normalize(extra1.xyz); // light-to-surface
    float3 toLight       = -lightDir;
    float3 baseColor     = float3(0.05f, 0.25f, 0.35f);
    float3 ambient        = 0.08f * baseColor;

    float ndotl   = max(dot(n, toLight), 0.0f);
    float3 diffuse = baseColor * ndotl * extra2.rgb * extra1.w;

    float3 viewDir = normalize(extra0.xyz - input.worldPos);
    float3 halfVec = normalize(toLight + viewDir);
    float specularPower = 64.0f;
    float specular = pow(max(dot(n, halfVec), 0.0f), specularPower) * 0.5f * extra1.w;

    // Schlick Fresnel. Water's normal-incidence reflectance is about 0.02 (IOR 1.33), rising to
    // ~1 at grazing angles, which is why a real water surface looks dark when you stare straight
    // down into it and mirror-like toward the horizon. Without this the surface reflects the same
    // everywhere and reads as flat plastic.
    float NoV = saturate(dot(n, viewDir));
    float fresnel = 0.02f + 0.98f * pow(1.0f - NoV, 5.0f);

    // Fresnel weights the specular (grazing angles reflect far more) and lifts the opacity the
    // same way: you see into the water from above and see the surface from the side.
    //
    // The reflected term is still only the sun's specular highlight -- there is no environment
    // reflection or refraction bound to this pass yet, so the horizon does not actually mirror the
    // sky. That needs SSR or IBL here, and refraction additionally needs the scene colour behind
    // the water, which this pass currently writes rather than reads.
    float3 color = ambient + diffuse * (1.0f - fresnel) + specular.xxx * (1.0f + 4.0f * fresnel);
    float alpha = lerp(0.75f, 0.98f, fresnel);

    // Refraction. Water transmits what is behind it, bent by the surface normal; the classic
    // screen-space approximation offsets the backdrop lookup by the normal's horizontal component,
    // which is where the bend points. Doing it properly would trace to the actual depth of what is
    // behind each pixel -- this offsets by a constant instead, so the distortion does not grow with
    // distance, but it costs one texture fetch and reads correctly for a surface this shallow.
    //
    // When the backdrop is bound the water composites itself: Fresnel splits the result between
    // what is transmitted (the refracted scene, tinted by the water) and what is reflected, and the
    // output is opaque. Without it the pass keeps the old behaviour and lets the hardware blend
    // show the scene through undistorted.
    if (extra3.x > 0.5f) {
        float2 screenUv = input.svPosition.xy * float2(1.0f / max(extra3.z, 1.0f), 1.0f / max(extra3.w, 1.0f));
        float2 refractOffset = n.xz * extra3.y;
        float3 transmitted = BackdropTex.SampleLevel(BackdropSampler, screenUv + refractOffset, 0).rgb;

        // Absorption by the water body. Water is not a neutral filter: its absorption coefficient
        // is roughly two orders of magnitude higher at 650 nm than at 450 nm, which is the whole
        // reason a body of water is blue-green rather than grey. A single transmittance colour
        // stands in for Beer-Lambert over an unknown path -- the path length would need the depth
        // of the bottom, which this pass does not have.
        //
        // The first attempt used lerp(1, baseColor * 4, 0.35), which works out to (0.72, 1.0, 1.14)
        // -- barely a tint at all -- and the water came out looking like plain glass.
        const float3 waterTransmittance = float3(0.30f, 0.72f, 0.82f);
        transmitted *= waterTransmittance;

        // Fresnel splits transmitted against reflected, and at grazing angles Fresnel is ~1, so
        // whatever the reflected term holds is what a distant water surface looks like. It used to
        // be ambient plus the sun highlight, which meant the horizon reflected darkness -- exactly
        // backwards, since that is where real water is brightest and most mirror-like.
        //
        // The environment here is the analytic sky rather than a cubemap or screen-space trace:
        // this pass runs right after the skybox and before the SSR passes, so an SSR result does
        // not exist yet, and an IBL cubemap is only present when the scene ships one. The trade is
        // that a scene using a *textured* skybox reflects the procedural sky instead of its own
        // texture; binding Skybox's cubemap would fix that and needs an accessor it does not expose.
        float3 reflectDir = reflect(-viewDir, n);
        // Rays bent below the horizon would sample the sky function's ground colour and put a dark
        // band on the water; a real surface that shallow reflects along the horizon instead.
        reflectDir.y = abs(reflectDir.y);
        const float3 skyReflection = ComputeSkyColor(reflectDir, toLight, extra2.rgb, extra1.w);

        const float3 reflected = skyReflection + specular.xxx * 5.0f;
        color = lerp(transmitted, reflected, fresnel) + diffuse * 0.35f;
        alpha = 1.0f;
    }

    // Foam is air trapped in water: a dense scattering medium, so it is bright, nearly opaque and
    // almost entirely diffuse. Folding it in after the water shading rather than tinting baseColor
    // matters -- foam must not keep the water's Fresnel-driven specular, or a whitecap ends up
    // with a mirror highlight sitting on it.
    float foam = saturate((input.agitation - kFoamThreshold) / kFoamRange);
    if (foam > 0.0f) {
        // Slightly off-white and lit diffusely; pure white reads as a hole punched in the surface.
        float3 foamColor = float3(0.92f, 0.95f, 0.97f) * (0.35f + 0.65f * ndotl) * extra2.rgb * extra1.w;

        // Spray: the violent tip of the range stops being matte foam and blows out.
        float spray = saturate((input.agitation - kSprayThreshold) / kFoamRange);
        foamColor += spray * 0.8f;

        color = lerp(color, foamColor, foam);
        alpha = lerp(alpha, 1.0f, foam);
    }

    return float4(color, alpha);
}
