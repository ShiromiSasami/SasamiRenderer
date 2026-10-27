// LensFlareRayTraced_VS.hlsl -- ghost vertices from an actual traced lens stack.
//
// Follows CAPCOM Open Conference Professional 2023, "Ray Tracing Lens Flare" (RE Engine team).
// Where LensFlare_PS.hlsl's two modes place ghosts either by hand or by a hashed pseudo-random
// scalar along the light vector, this mode does neither: the lens is modelled as a sequence of
// planar and spherical surfaces (ray/quadric intersection is closed-form, so tracing dozens of
// rays per ghost per frame is cheap), a grid is cast through the front element, and wherever that
// grid lands on the sensor -- distorted by the glass it passed through -- IS the ghost's shape.
// Nothing about a ghost's position, size or silhouette is authored or randomised; it all falls
// out of the optics.
//
// A ghost is faint surface reflection that still reaches the sensor, which is only possible after
// an even number of reflections: refract forward to a far surface j, reflect, refract backward to
// a near surface i, reflect again, then refract forward the rest of the way out. Every (i, j) pair
// with i < j is a distinct ghost; SV_InstanceID selects which pair this draw call is tracing (see
// DecodeGhostPath). Intensity is the product of the two surfaces' Fresnel reflectances, and the
// ray's position where it crosses the aperture stop is recorded so the pixel shader can clip the
// ghost to the diaphragm's shape -- both exactly as the talk describes recording during the trace.
//
// Simplifications versus a real optical design, called out here rather than left implicit:
//  - The lens table below is representative -- plausible double-Gauss-like radii and spacings --
//    not a manufacturer's prescription. It is the one place to edit for a different lens.
//  - One refractive index per glass, i.e. no chromatic dispersion; a real lens flare shows
//    coloured fringing on ghosts from wavelength-dependent index, which this does not reproduce.
//  - AR coatings are reduced to Schlick's unpolarised-light reflectance approximation per
//    surface, not a spectral thin-film model.
//  - The entry grid is a plain square over the front element's clear aperture, not pupil-weighted
//    or mapped through a disc parameterisation; points outside the physical aperture just vignette
//    to zero intensity instead of being excluded from the mesh.
//  - No per-quad energy conservation: a full implementation would weight brightness by how much
//    the grid mapping compresses or stretches area (its Jacobian) so ghosts conserve irradiance;
//    here brightness is the Fresnel product alone (see LensFlareRayTraced_PS.hlsl).
//  - Ray/sphere root selection always takes the nearer forward intersection, which is only valid
//    for the near-axis field angles a flare uses; a general lens-design tracer would need to
//    disambiguate the two roots more carefully.

cbuffer LensFlareCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_flareParams;         // xy: light position in UV space, z: intensity, w: aspect ratio (w/h)
    float4 u_flareTint;           // rgb: light colour, w: aperture blade count
    float4 u_flareMode;           // x: mode, y: grid resolution N (vertices per side), z: ghost path count
};

// A lens surface is either a sphere (curvatureRadius != 0) or a plane (curvatureRadius == 0),
// per the talk's "the lens is a sequence of planes and spheres" simplification -- both are cheap,
// closed-form ray intersections, unlike a general aspheric surface.
struct LensSurface
{
    float curvatureRadius; // signed radius of curvature, mm-scale; 0 = flat (e.g. the aperture stop)
    float axialPosition;   // position along the optical axis z, front element vertex at z = 0
    float apertureRadius;  // clear radius (semi-diameter); rays landing outside this are vignetted
    float indexAfter;      // refractive index of the medium immediately following this surface
};

static const uint kSurfaceCount = 11u;
static const uint kApertureStopIndex = 5u;

// Representative double-Gauss-like stack, NOT a real manufacturer's prescription -- radii,
// spacings and glass indices are plausible round numbers chosen so the trace produces believable
// ghosts. This array is the single place to edit to try a different lens design.
//                                 R (mm)     z (mm)    semi-aperture (mm)  n after
static const LensSurface kLensSurfaces[kSurfaceCount] = {
    {  55.0f,     0.0f,     22.0f,               1.62f },  // 0: front element, front face (air -> glass)
    { 220.0f,     9.0f,     21.0f,               1.00f },  // 1: front element, back face (glass -> air)
    {  35.0f,    15.0f,     18.0f,               1.58f },  // 2: second element, front face (air -> glass)
    {  20.0f,    21.0f,     16.0f,               1.65f },  // 3: cemented doublet interface (glass -> glass)
    {  60.0f,    26.0f,     15.0f,               1.00f },  // 4: doublet, back face (glass -> air)
    {   0.0f,    34.0f,     12.0f,               1.00f },  // 5: APERTURE STOP -- flat diaphragm, index unchanged
    { -55.0f,    42.0f,     15.0f,               1.60f },  // 6: rear group, front face (air -> glass)
    { -30.0f,    47.0f,     16.0f,               1.55f },  // 7: rear cemented interface (glass -> glass)
    { -90.0f,    53.0f,     18.0f,               1.00f },  // 8: rear element, back face (glass -> air)
    {-180.0f,    60.0f,     20.0f,               1.63f },  // 9: rearmost element, front face (air -> glass)
    {-400.0f,    66.0f,     22.0f,               1.00f },  // 10: rearmost element, back face -> image space (air)
};

static const float kSensorZ = 106.0f;              // back focal distance, clear of the last element
// Half of the sensor's shorter side, in the same millimetres as the table above -- but treat it as
// the projection scale it actually is here, not as a physical sensor. The surface data above is
// representative rather than a real design, so the system does not truly focus: rays arrive at the
// image plane still spanning most of the entrance pupil. A physical 18 mm therefore mapped a single
// ghost across the whole frame (verified on screen). Enlarging it brings the ghosts down to the
// fraction of the frame real ones occupy. Replacing the table with a genuine prescription is what
// would make this value physical again.
static const float kSensorHalfSize = 85.0f;
static const float kFrontStandoff = 2.0f;          // small gap the entry rays start from, ahead of surface 0
static const float kMaxFieldAngleRadians = 0.35f;  // ~20 degree half-field at the frame edge; representative only

static const uint kTotalGhostPaths = kSurfaceCount * (kSurfaceCount - 1u) / 2u;

// Walks every (i, j), i < j < kSurfaceCount pair in a fixed row-major triangular order:
// (0,1), (0,2), ..., (0,N-1), (1,2), ..., (N-2,N-1). Deterministic, so the CPU only needs to send
// how many ghost paths to draw (u_flareMode.z, capped at kTotalGhostPaths), never a table of which
// surface pairs those are.
void DecodeGhostPath(uint pathIndex, out uint i, out uint j)
{
    pathIndex = pathIndex % max(kTotalGhostPaths, 1u);

    uint row = 0u;
    uint rowLength = kSurfaceCount - 1u;
    [loop]
    while (pathIndex >= rowLength && rowLength > 0u)
    {
        pathIndex -= rowLength;
        row += 1u;
        rowLength -= 1u;
    }

    i = row;
    j = row + 1u + pathIndex;
}

// Ray/quadric intersection: closed-form for both the flat and spherical cases, per the talk's
// surface model. `normal` is returned facing against the incoming ray so refract()/reflect() can
// use it directly regardless of which direction the ray is currently travelling.
bool IntersectSurface(float3 rayOrigin, float3 rayDir, float curvatureRadius, float axialZ,
                       out float3 hitPoint, out float3 normal)
{
    hitPoint = rayOrigin;
    normal = float3(0.0f, 0.0f, 1.0f);

    if (abs(curvatureRadius) < 1e-6f)
    {
        // Flat surface (the diaphragm plane): a plane/ray intersection.
        if (abs(rayDir.z) < 1e-6f)
        {
            return false;
        }
        const float t = (axialZ - rayOrigin.z) / rayDir.z;
        if (t < 1e-4f)
        {
            return false;
        }
        hitPoint = rayOrigin + rayDir * t;
        normal = float3(0.0f, 0.0f, -sign(rayDir.z));
        return true;
    }

    // Sphere centred on the optical axis, vertex at (0, 0, axialZ), radius |curvatureRadius|.
    const float3 center = float3(0.0f, 0.0f, axialZ + curvatureRadius);
    const float3 oc = rayOrigin - center;
    const float b = dot(oc, rayDir);
    const float c = dot(oc, oc) - curvatureRadius * curvatureRadius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return false;
    }

    const float sq = sqrt(discriminant);
    const float t0 = -b - sq;
    const float t1 = -b + sq;

    // Take the nearer forward root. A general lens-design tracer would need to disambiguate the
    // two sphere roots more carefully; for the near-axis field angles a flare uses, the nearer
    // positive root is always the physically correct surface crossing.
    float t = 1.0e30f;
    bool found = false;
    if (t0 > 1e-4f) { t = t0; found = true; }
    if (t1 > 1e-4f && t1 < t) { t = t1; found = true; }
    if (!found)
    {
        return false;
    }

    hitPoint = rayOrigin + rayDir * t;
    normal = normalize(hitPoint - center);
    if (dot(normal, rayDir) > 0.0f)
    {
        normal = -normal;
    }
    return true;
}

// Schlick's approximation for unpolarised light, not the full Fresnel equations -- a real AR
// coating's reflectance also depends on wavelength and coating thickness, neither of which this
// traces; every ghost surface here uses one achromatic reflectance number.
float FresnelSchlick(float cosTheta, float n1, float n2)
{
    float r0 = (n1 - n2) / (n1 + n2);
    r0 *= r0;
    const float x = saturate(1.0f - cosTheta);
    const float x5 = x * x * x * x * x;
    return r0 + (1.0f - r0) * x5;
}

// Refracts through surfaces [surfaceLo, surfaceHi) while travelling in +z. `currentIor` is the
// medium the ray is in when it reaches surfaceLo, and is updated to the medium after surfaceHi-1
// on return. Used both for the initial entry into the lens and for the final leg out to the
// sensor -- the two forward legs either side of a ghost's reflection pair.
void RefractForward(inout float3 rayOrigin, inout float3 rayDir, inout float currentIor,
                     uint surfaceLo, uint surfaceHi, inout float2 aperturePosition, inout float intensity)
{
    [loop]
    for (uint s = surfaceLo; s < surfaceHi; ++s)
    {
        float3 hitPoint, normal;
        if (!IntersectSurface(rayOrigin, rayDir, kLensSurfaces[s].curvatureRadius, kLensSurfaces[s].axialPosition, hitPoint, normal))
        {
            intensity = 0.0f;
            return;
        }

        if (length(hitPoint.xy) > kLensSurfaces[s].apertureRadius)
        {
            // Blocked by the lens barrel. Kept as a zero-intensity vertex rather than an early
            // return so the ghost mesh's topology stays intact for the rasteriser.
            intensity = 0.0f;
        }
        if (s == kApertureStopIndex)
        {
            aperturePosition = hitPoint.xy;
        }

        const float nextIor = kLensSurfaces[s].indexAfter;
        float3 refracted = refract(rayDir, normal, currentIor / nextIor);
        if (dot(refracted, refracted) < 1e-8f)
        {
            // Total internal reflection: past the critical angle no light refracts through.
            // Treated as a (lossless, for simplicity) reflection rather than the NaN direction
            // HLSL's refract() would otherwise hand back.
            refracted = reflect(rayDir, normal);
        }
        rayDir = normalize(refracted);
        rayOrigin = hitPoint;
        currentIor = nextIor;
    }
}

// Mirror of RefractForward for the return leg of a ghost path, between its two reflecting
// surfaces: walks strictly between surfaceHiExclusive and surfaceLoExclusive while travelling -z.
void RefractBackward(inout float3 rayOrigin, inout float3 rayDir, inout float currentIor,
                      uint surfaceHiExclusive, uint surfaceLoExclusive, inout float2 aperturePosition, inout float intensity)
{
    [loop]
    for (int s = int(surfaceHiExclusive) - 1; s > int(surfaceLoExclusive); --s)
    {
        const uint su = uint(s);
        float3 hitPoint, normal;
        if (!IntersectSurface(rayOrigin, rayDir, kLensSurfaces[su].curvatureRadius, kLensSurfaces[su].axialPosition, hitPoint, normal))
        {
            intensity = 0.0f;
            return;
        }

        if (length(hitPoint.xy) > kLensSurfaces[su].apertureRadius)
        {
            intensity = 0.0f;
        }
        if (su == kApertureStopIndex)
        {
            aperturePosition = hitPoint.xy;
        }

        const float before = (su == 0u) ? 1.0f : kLensSurfaces[su - 1u].indexAfter;
        float3 refracted = refract(rayDir, normal, currentIor / before);
        if (dot(refracted, refracted) < 1e-8f)
        {
            refracted = reflect(rayDir, normal);
        }
        rayDir = normalize(refracted);
        rayOrigin = hitPoint;
        currentIor = before;
    }
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 aperturePosition : TEXCOORD0; // hit position on the aperture stop, normalised by its radius
    float intensity : TEXCOORD1;         // accumulated Fresnel product for this ghost's reflection pair
    float2 gridCoord : TEXCOORD2;        // the entrance-pupil coordinate this vertex came from
};

VSOutput VSMain(float2 gridCoord : POSITION0, uint instanceID : SV_InstanceID)
{
    VSOutput output;

    uint i, j;
    DecodeGhostPath(instanceID, i, j);

    const float aspect = max(u_flareParams.w, 0.0001f);

    // Off-axis field angle from the light's screen position -- this, not any randomisation, is
    // what makes a ghost move as the light moves, since every grid ray shares the same incoming
    // direction (a distant point light) but the direction itself is the light's own bearing.
    float2 fieldOffset = (u_flareParams.xy - 0.5f) * 2.0f;
    fieldOffset.x *= aspect;
    const float2 fieldAngle = fieldOffset * kMaxFieldAngleRadians;
    const float3 incomingDir = normalize(float3(tan(fieldAngle.x), tan(fieldAngle.y), 1.0f));

    // Entrance-pupil point: centre the [0,1] grid and scale by the front element's clear radius.
    const float2 entryXY = (gridCoord * 2.0f - 1.0f) * kLensSurfaces[0].apertureRadius;

    float3 rayOrigin = float3(entryXY, kLensSurfaces[0].axialPosition - kFrontStandoff);
    float3 rayDir = incomingDir;
    float currentIor = 1.0f; // object space is air
    float intensity = 1.0f;
    float2 aperturePosition = float2(0.0f, 0.0f);

    // Pass 1: refract forward from the front element up to (but not through) the far surface j.
    RefractForward(rayOrigin, rayDir, currentIor, 0u, j, aperturePosition, intensity);

    // Reflect at j -- the far surface of the ghost's pair. Only a fraction of a percent of the
    // light bounces here; the rest continues through and belongs to a different ghost (or none).
    {
        float3 hitPoint, normal;
        if (IntersectSurface(rayOrigin, rayDir, kLensSurfaces[j].curvatureRadius, kLensSurfaces[j].axialPosition, hitPoint, normal))
        {
            if (length(hitPoint.xy) > kLensSurfaces[j].apertureRadius)
            {
                intensity = 0.0f;
            }
            if (j == kApertureStopIndex)
            {
                aperturePosition = hitPoint.xy;
            }
            const float cosIncidence = abs(dot(normal, rayDir));
            intensity *= FresnelSchlick(cosIncidence, currentIor, kLensSurfaces[j].indexAfter);
            rayDir = reflect(rayDir, normal);
            rayOrigin = hitPoint;
            // Reflection does not change which medium the ray is in -- it bounces back into the
            // medium it approached from, so currentIor is left untouched here.
        }
        else
        {
            intensity = 0.0f;
        }
    }

    // Pass 2: refract backward from j-1 down to i+1, against the direction of travel.
    RefractBackward(rayOrigin, rayDir, currentIor, j, i, aperturePosition, intensity);

    // Reflect at i -- the near surface of the pair -- sending the ray forward again.
    {
        float3 hitPoint, normal;
        if (IntersectSurface(rayOrigin, rayDir, kLensSurfaces[i].curvatureRadius, kLensSurfaces[i].axialPosition, hitPoint, normal))
        {
            if (length(hitPoint.xy) > kLensSurfaces[i].apertureRadius)
            {
                intensity = 0.0f;
            }
            if (i == kApertureStopIndex)
            {
                aperturePosition = hitPoint.xy;
            }
            const float mediumBeforeI = (i == 0u) ? 1.0f : kLensSurfaces[i - 1u].indexAfter;
            const float cosIncidence = abs(dot(normal, rayDir));
            intensity *= FresnelSchlick(cosIncidence, currentIor, mediumBeforeI);
            rayDir = reflect(rayDir, normal);
            rayOrigin = hitPoint;
            // As at j, reflection keeps the ray in the medium it approached from.
        }
        else
        {
            intensity = 0.0f;
        }
    }

    // Pass 3: refract forward through the remaining surfaces, i+1 .. last, out to the sensor.
    RefractForward(rayOrigin, rayDir, currentIor, i + 1u, kSurfaceCount, aperturePosition, intensity);

    // Sensor plane intersection -- the same maths as IntersectSurface's flat branch, inlined
    // because only the raw hit position is needed here, not a surface normal.
    float3 sensorHit = rayOrigin;
    if (abs(rayDir.z) > 1e-6f)
    {
        const float t = (kSensorZ - rayOrigin.z) / rayDir.z;
        sensorHit = rayOrigin + rayDir * t;
    }
    else
    {
        intensity = 0.0f;
    }

    // Sensor -> NDC: the sensor plane is flat and perpendicular to the axis, so this is a plain
    // linear divide by its half-size, not a projection matrix -- there is no camera in this pass,
    // only the lens. Aspect is applied on x exactly as the contract specifies.
    float2 ndc = sensorHit.xy / kSensorHalfSize;
    ndc.x /= aspect;

    output.position = float4(ndc, 0.0f, 1.0f);
    // Normalised by the stop's own radius so the pixel shader tests against a canonical disc and
    // never needs this file's physical lens numbers duplicated into it.
    output.aperturePosition = aperturePosition / max(kLensSurfaces[kApertureStopIndex].apertureRadius, 0.0001f);
    output.intensity = max(intensity, 0.0f);

    // Carried so the pixel shader can measure how far the trace stretched this part of the grid.
    // A ghost that spreads over more pixels must be dimmer per pixel -- the energy that entered
    // through a fixed patch of the entrance pupil is the same however far it fans out. Without
    // that, the ghosts that land compressed near the light pile up additively and blow out.
    output.gridCoord = gridCoord;
    return output;
}
