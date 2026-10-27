// SmokeVolume_PS.hlsl
//
// Raymarches SmokeVolumeSim's density field and blends it over SceneColor. The raymarch shape
// (reconstruct ray from screen UV + inverse VP, intersect a volume bound, step front-to-back
// accumulating Beer-Lambert absorption, early-out once transmittance is negligible) follows the
// volumetric cloud pass already in Shaders/Effects/Volumetric/Cloud/VolumetricCloud_PS.hlsl --
// the difference here is the "cloud" is a finite axis-aligned box read from a structured buffer
// instead of an infinite horizontal slab sampled procedurally.
//
// This pass does NOT do self-shadowing or in-scattering: the smoke colour below is a flat grey
// modulated only by how much of the volume the camera ray has already crossed. Marching a second
// (shadow) ray toward a light per step, the way VolumetricCloud_PS does for its sun, is a
// separate, later item.
//
// Camera CB layout (b0), matching SmokeVolumeRenderPass::CreatePipeline's PushCameraCB call:
//   u_invViewProj : inverse view-projection, for reconstructing the world-space ray from UV.
//   u_viewProj    : FORWARD view-projection. Used only to re-project the ray/box near-hit point
//                   back to NDC depth, written out via SV_Depth below. A fullscreen triangle has
//                   one flat interpolated depth, which cannot by itself occlude a volume that
//                   sits at varying depth across the screen against real scene geometry -- so the
//                   depth the hardware test uses has to come from the pixel shader instead.
//   u_camPos      : camera world position (xyz). The true ray origin -- unprojecting the near
//                   plane instead would offset the origin by the near-clip distance, which
//                   matters at the scale of this volume (a few metres).
//   u_extra1      : reserved, currently zero.

cbuffer SmokeCameraCB : register(b0)
{
    row_major float4x4 u_invViewProj;
    row_major float4x4 u_viewProj;
    float4             u_camPos;  // xyz = camera world position
    float4             u_extra1;  // xyz = directional light direction (light-to-surface), w = intensity
    float4             u_extra2;  // rgb = directional light colour
};

// Mirrors SmokeVolumeSim::SmokeUpdateCBData byte-for-byte; only the grid geometry fields
// (origin/cellSize/count) are used here, the rest is the simulation's own update state.
cbuffer SmokeUpdateCB : register(b1)
{
    float    originX;
    float    originY;
    float    originZ;
    float    cellSize;

    uint     countX;
    uint     countY;
    uint     countZ;
    float    dt;

    float    emitterX;
    float    emitterY;
    float    emitterZ;
    float    emitterRadius;

    float    emitterDensity;
    float    emitterVelocity;
    float    buoyancy;
    float    dissipation;

    float    velocityDamping;
    float    vorticity;
    uint     emitterActive;
    uint     pad0;
};

// (velocityX, velocityY, velocityZ, density) per cell; only .w is read here. Index is
// x + y*countX + z*countX*countY, matching SmokeVolumeSim's C++/compute-shader convention.
StructuredBuffer<float4> g_state : register(t0);

// The scene's depth buffer. Used to stop the march at whatever stands inside the volume:
// without it the smoke simply draws over any geometry the box encloses.
Texture2D<float> SceneDepthTex : register(t1);
SamplerState DepthSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

struct PSOutput
{
    float4 color : SV_Target0;
};

// Nearest-clamped fetch. Cells outside the grid repeat the border cell rather than reading
// garbage or wrapping, so the box's outer half-cell shell does not fade to zero density.
float FetchDensity(int3 idx, int3 maxIdx)
{
    idx = clamp(idx, int3(0, 0, 0), maxIdx);
    uint linearIdx = (uint)idx.x + (uint)idx.y * countX + (uint)idx.z * (countX * countY);
    return g_state[linearIdx].w;
}

// Hand-rolled trilinear: the buffer is a plain StructuredBuffer (see SmokeVolumeSim.h for why --
// ping-pong plus Jacobi pressure iterations, not hardware texture filtering), so there is no
// bilinear/trilinear sampler doing this for free the way there would be off a 3D texture.
// Henyey-Greenstein phase function. Smoke scatters strongly forward, which is why a plume lit
// from behind glows and the same plume lit from the front reads flat -- a phase function is what
// makes that difference appear at all. Same formulation the volumetric cloud pass uses.
float PhaseHG(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * max(denom * sqrt(max(denom, 1e-6f)), 1e-6f));
}

float SampleDensityTrilinear(float3 worldPos)
{
    float3 originV = float3(originX, originY, originZ);
    // Cell i's density represents that cell's centre, i.e. world position (i+0.5)*cellSize from
    // the origin. Shifting by -0.5 before splitting into integer + fractional parts re-centres the
    // interpolation on those cell centres instead of snapping a half-cell off everywhere.
    float3 cellCoord = (worldPos - originV) / cellSize - 0.5f;
    int3   i0 = int3(floor(cellCoord));
    float3 f  = cellCoord - float3(i0);

    int3 maxIdx = int3((int)countX, (int)countY, (int)countZ) - int3(1, 1, 1);

    float c000 = FetchDensity(i0 + int3(0, 0, 0), maxIdx);
    float c100 = FetchDensity(i0 + int3(1, 0, 0), maxIdx);
    float c010 = FetchDensity(i0 + int3(0, 1, 0), maxIdx);
    float c110 = FetchDensity(i0 + int3(1, 1, 0), maxIdx);
    float c001 = FetchDensity(i0 + int3(0, 0, 1), maxIdx);
    float c101 = FetchDensity(i0 + int3(1, 0, 1), maxIdx);
    float c011 = FetchDensity(i0 + int3(0, 1, 1), maxIdx);
    float c111 = FetchDensity(i0 + int3(1, 1, 1), maxIdx);

    float c00 = lerp(c000, c100, f.x);
    float c10 = lerp(c010, c110, f.x);
    float c01 = lerp(c001, c101, f.x);
    float c11 = lerp(c011, c111, f.x);

    float c0 = lerp(c00, c10, f.y);
    float c1 = lerp(c01, c11, f.y);

    return lerp(c0, c1, f.z);
}

// Slab method (Kay & Kajiya) against the volume's world-space AABB. A zero ray-direction
// component divides to +-inf, which min/max below resolve correctly without a branch.
bool IntersectBox(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax,
                   out float tNear, out float tFar)
{
    float3 invDir = 1.0f / rayDir;
    float3 t0 = (boxMin - rayOrigin) * invDir;
    float3 t1 = (boxMax - rayOrigin) * invDir;
    float3 tMin = min(t0, t1);
    float3 tMax = max(t0, t1);
    tNear = max(max(tMin.x, tMin.y), tMin.z);
    tFar  = min(min(tMax.x, tMax.y), tMax.z);
    return tFar > max(tNear, 0.0f);
}

PSOutput PSMain(PSInput input)
{
    PSOutput o;

    // Reconstruct world-space ray direction from screen UV, same construction as
    // VolumetricCloud_PS: unproject the near and far NDC points and take the difference. Reverse-
    // Z: near = 1.0, far = 0.0, so the NDC z literals are swapped relative to a forward-Z renderer.
    float2 ndcXY = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    float4 nearH = mul(float4(ndcXY, 1.0f, 1.0f), u_invViewProj);
    float4 farH  = mul(float4(ndcXY, 0.0f, 1.0f), u_invViewProj);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f) {
        o.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;
    }
    float3 nearPt = nearH.xyz / nearH.w;
    float3 farPt  = farH.xyz / farH.w;
    float3 rayDir = normalize(farPt - nearPt);

    // The true camera position, not nearPt: the near clip plane sits some distance in front of the
    // eye, and that offset is not negligible against a volume only a few metres across.
    float3 rayOrigin = u_camPos.xyz;

    float3 boxMin = float3(originX, originY, originZ);
    float3 boxMax = boxMin + float3((float)countX, (float)countY, (float)countZ) * cellSize;

    float tNear, tFar;
    if (!IntersectBox(rayOrigin, rayDir, boxMin, boxMax, tNear, tFar)) {
        o.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;
    }
    tNear = max(tNear, 0.0f); // camera may already be inside the box

    // Re-project the box's near-hit point to NDC depth and hand it to the hardware depth test
    // (write is disabled on this pipeline, so this cannot corrupt SceneDepth). Without this, a
    // pixel where opaque geometry sits in front of the box would still pass the GreaterEqual test
    // against the triangle's own flat depth and the smoke would incorrectly draw on top of it.
    float3 hitPos = rayOrigin + rayDir * tNear;
    float4 hitClip = mul(float4(hitPos, 1.0f), u_viewProj);

    // Stop at the scene. Reconstructing the world position of whatever the depth buffer holds for
    // this pixel and taking its distance along the ray gives the exact point the march must not pass
    // -- geometry standing inside the volume then occludes the smoke behind it, which a single
    // per-fragment depth test could never express for a fullscreen triangle.
    //
    // This renderer is reverse-Z: near = 1, far = 0, cleared to 0. A depth of 0 therefore means
    // "nothing was drawn here", not "infinitely close", so it must not clamp anything.
    {
        const float sceneDepth = SceneDepthTex.SampleLevel(DepthSampler, input.uv, 0);
        if (sceneDepth > 0.0f) {
            const float4 sceneClip = float4(ndcXY, sceneDepth, 1.0f);
            const float4 sceneWorldH = mul(sceneClip, u_invViewProj);
            if (abs(sceneWorldH.w) > 1e-7f) {
                const float3 sceneWorld = sceneWorldH.xyz / sceneWorldH.w;
                tFar = min(tFar, dot(sceneWorld - rayOrigin, rayDir));
            }
        }
    }
    if (tFar <= max(tNear, 0.0f)) {
        o.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return o;   // the scene occludes the whole span
    }

    const int   kSteps = 56; // within the spec's 48-64 range
    const float kAbsorption = 1.5f; // extinction coefficient scale; tuned by eye, not simulated

    // Self-shadowing: at every lit step, march a short way toward the sun and accumulate the
    // density in the way. Without it the plume is uniformly grey and reads as fog rather than a
    // solid body -- the near side being lit while the far side falls into shadow is most of what
    // makes smoke look like smoke. Following the volumetric cloud pass, which does the same thing
    // with the same step count.
    const int   kShadowSteps = 6;
    const float kShadowSpan = 0.9f;   // world units marched toward the light in total

    float tSpan = tFar - tNear;
    float stepLength = tSpan / (float)kSteps;

    float  transmittance = 1.0f;
    float3 colorAccum = float3(0.0f, 0.0f, 0.0f);

    // Smoke's own albedo. Soot is dark; what makes a plume look bright is scattered sunlight, not
    // a bright body colour, so this stays low and the light term does the work.
    const float3 kSmokeAlbedo = float3(0.62f, 0.62f, 0.65f);
    // Ambient stands in for the sky lighting the plume from every direction; without it the
    // shadowed side goes to pure black, which no real smoke does.
    const float3 kAmbient = float3(0.22f, 0.25f, 0.30f);

    const float3 toLight = normalize(-u_extra1.xyz);
    const float  sunIntensity = u_extra1.w;
    const float3 sunColor = u_extra2.rgb;

    // Forward-scattering lobe blended with a weaker backward one: a single HG lobe cannot produce
    // both the bright rim when looking toward the sun and the soft fill when looking away.
    const float  cosTheta = dot(rayDir, toLight);
    const float  phase = lerp(PhaseHG(cosTheta, 0.55f), PhaseHG(cosTheta, -0.2f), 0.35f);

    const float shadowStep = kShadowSpan / (float)kShadowSteps;

    for (int i = 0; i < kSteps; ++i)
    {
        float  t   = tNear + ((float)i + 0.5f) * stepLength;
        float3 pos = rayOrigin + rayDir * t;

        float density = SampleDensityTrilinear(pos);
        if (density > 1e-4f)
        {
            float sigma       = density * kAbsorption;
            float sampleT     = exp(-sigma * stepLength);
            float sampleAlpha = 1.0f - sampleT;

            // Shadow march. Six taps over a short span rather than to the volume edge: the
            // contribution falls off exponentially, so the far half of a long march changes
            // nothing while costing the same.
            float shadowTau = 0.0f;
            [unroll]
            for (int j = 0; j < kShadowSteps; ++j) {
                const float3 shadowPos = pos + toLight * (((float)j + 0.5f) * shadowStep);
                shadowTau += SampleDensityTrilinear(shadowPos);
            }
            const float lightTransmittance = exp(-shadowTau * kAbsorption * shadowStep);

            // Powder term: darkens dense interiors relative to wispy edges, approximating the
            // multiple scattering a single-scatter march cannot represent.
            const float powder = 1.0f - exp(-sigma * stepLength * 2.0f);

            const float3 scattered = sunColor * sunIntensity * lightTransmittance * phase * powder * 2.0f;
            const float3 smokeColor = kSmokeAlbedo * (scattered + kAmbient);
            colorAccum += transmittance * sampleAlpha * smokeColor;

            transmittance *= sampleT;
            if (transmittance < 0.01f) {
                break; // early-out: negligible remaining contribution
            }
        }
    }

    o.color = float4(colorAccum, 1.0f - transmittance); // premultiplied
    return o;
}
