// LensFlare_PS.hlsl -- ghost-based lens flare with randomised placement.
//
// Follows Masaki Kawase (Silicon Studio), "物理ベース？アート指向？ ～尤もらしさと自由度を
// 両立するレンズフレア表現～", CEDEC 2019. The talk's position is that neither hand-authored
// sprites (free, but fixed and blind to zoom/aperture) nor full lens simulation (Hullin 2011,
// Lee 2013 -- accurate but data-hungry and hard to art-direct) is what a game wants, and that
// physical accuracy can be dropped as long as the result is plausible.
//
// Its central practical claim, and the reason this shader exists: the ghosts do not need to be
// placed by hand. Position is g = s * l for a per-ghost scalar s, and "s は基本的に乱数で良い"
// -- only the few ghosts worth art-directing get authored values. Size likewise is "実用的には
// 殆ど乱数で良い". The one thing that must not be plain uniform noise is the positional scatter:
// real ghosts are not isotropic, and uniform jitter reads as wrong, so the random value is
// raised to a power first so that only a few ghosts deviate far.
//
// Everything per-ghost here is therefore derived from a hash of the ghost index: the CPU sends
// one light position and a count, not a table.

Texture2D InputTex : register(t0);
SamplerState LinearWrap : register(s0);

cbuffer LensFlareCB : register(b0)
{
    row_major float4x4 u_unused0; // camera-CB layout padding, unused by this pass
    row_major float4x4 u_unused1; // camera-CB layout padding, unused by this pass
    float4 u_flareParams;         // xy: light position in UV space, z: intensity, w: aspect ratio (w/h)
    float4 u_flareTint;           // rgb: light colour, w: aperture blade count
    float4 u_flareMode;           // x: 0 = static sprite, 1 = Silicon Studio procedural
};

static const int   kGhostCount  = 12;
// s clusters near -1 -- the point-symmetric position of the light -- and spreads out to roughly
// +-2..4 (talk's stated range).
static const float kGhostSpread = 3.0f;
// Uniform jitter looks wrong; the exponent leaves most ghosts near their nominal position and
// lets a few wander. 3.0 is the mildest power that still reads as "a few outliers".
static const float kJitterPower = 3.0f;
static const float kJitterScale = 0.22f;
// Edge softness in distance-field units. Constant rather than screen-derived: the ghosts are
// procedural, so there is no texel to match.
static const float kEdgeSoftness = 0.015f;

// Mode 0: the classic sprite table. Every ghost's offset, size and tint is authored by hand, which
// is how lens flare was done before anyone modelled the optics -- free and art-directable, but the
// table is fixed, so the flare cannot answer to aperture, zoom or anything else the camera does.
// Reproduced here for comparison; the point is what it does NOT do next to the other two modes.
//
// The numbers are the shape a hand-authored set usually takes: a couple of big soft ghosts near the
// point-symmetric position, a chain of small ones marching past it, one past the light itself.
static const int kSpriteGhostCount = 8;
static const float3 kSpriteGhosts[kSpriteGhostCount] = {
    //     s        radius   tint index (0 = warm, 1 = cool)
    float3(-1.00f,  0.090f,  0.0f),
    float3(-0.70f,  0.045f,  1.0f),
    float3(-0.40f,  0.028f,  0.0f),
    float3(-0.15f,  0.020f,  1.0f),
    float3( 0.30f,  0.034f,  0.0f),
    float3( 0.65f,  0.055f,  1.0f),
    float3( 1.30f,  0.075f,  0.0f),
    float3( 2.10f,  0.110f,  1.0f),
};

// Hash without sine: sin-based hashes differ between drivers and the ghost layout would not be
// reproducible across machines.
float Hash11(float n)
{
    uint x = asuint(n * 1.0f + 1.0f);
    x ^= x >> 17; x *= 0xed5ad4bbu;
    x ^= x >> 11; x *= 0xac4c1b51u;
    x ^= x >> 15; x *= 0x31848babu;
    x ^= x >> 14;
    return float(x) * (1.0f / 4294967296.0f);
}

float2 Hash21(float n)
{
    return float2(Hash11(n), Hash11(n + 71.3f));
}

// Regular polygon signed distance, after Inigo Quilez's 2D distance function collection
// (https://iquilezles.org/articles/distfunctions2d/) -- the same source the talk cites for its
// procedural aperture. Negative inside.
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

float4 PSMain(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    const float2 lightUv = u_flareParams.xy;
    const float aspect = max(u_flareParams.w, 0.0001f);

    // Work in an aspect-corrected space so ghosts stay round on a wide viewport.
    const float2 pixel = float2((uv.x - 0.5f) * aspect, uv.y - 0.5f);
    const float2 light = float2((lightUv.x - 0.5f) * aspect, lightUv.y - 0.5f);

    // Occlusion: the talk draws ghosts from a known light, but a light behind a wall must not
    // keep flaring. The tone-mapped image around the light's own position is a cheap stand-in
    // for a visibility query -- bright there means the light is actually visible. Four taps,
    // since a single one flickers on thin geometry.
    float visibility = 0.0f;
    {
        const float2 offsets[4] = {
            float2( 0.006f,  0.0f), float2(-0.006f,  0.0f),
            float2( 0.0f,  0.006f), float2( 0.0f, -0.006f),
        };
        [unroll]
        for (int t = 0; t < 4; ++t) {
            const float2 tapUv = saturate(lightUv + offsets[t]);
            const float3 tap = InputTex.SampleLevel(LinearWrap, tapUv, 0).rgb;
            visibility += max(max(tap.r, tap.g), tap.b);
        }
        visibility = saturate(visibility * 0.25f);
        // Only a near-saturated neighbourhood counts as "the light is visible"; lit geometry
        // sits well below that after tone mapping.
        visibility = saturate((visibility - 0.6f) / 0.4f);
    }
    if (visibility <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float bladeCount = max(u_flareTint.w, 3.0f);
    const float lightDistance = length(light);

    float3 accumulated = 0.0f.xxx;

    const bool proceduralMode = u_flareMode.x > 0.5f;
    const int ghostCount = proceduralMode ? kGhostCount : kSpriteGhostCount;

    [loop]
    for (int i = 0; i < ghostCount; ++i) {
        const float seed = float(i) * 13.0f + 1.0f;
        const float2 r0 = proceduralMode ? Hash21(seed) : float2(0.5f, 0.5f);
        const float2 r1 = proceduralMode ? Hash21(seed + 37.0f) : float2(0.5f, 0.5f);

        float s;
        float radius;
        float2 jitter = float2(0.0f, 0.0f);
        if (proceduralMode) {
            // g = s * l. s spans roughly -kGhostSpread..+kGhostSpread with the crowd near -1, and
            // is bent by a power of |l| because ghost tracks are not linear in field angle.
            s = lerp(-kGhostSpread, kGhostSpread, r0.x);
            s *= 1.0f + 0.35f * pow(saturate(lightDistance * 2.0f), 1.5f);

            // Power-biased scatter: most ghosts stay on the axis, a few leave it.
            const float2 jitterDir = normalize(r1 * 2.0f - 1.0f + 0.0001f.xx);
            jitter = jitterDir * pow(r0.y, kJitterPower) * kJitterScale;

            // "実用的には殆ど乱数で良い" -- size is random, but ghosts closer to screen centre
            // trend smaller, which is the behaviour the talk describes.
            radius = lerp(0.02f, 0.13f, r1.x);
        } else {
            // Straight from the table. No |l| term, no jitter: the sprite method's ghosts sit at
            // fixed multiples of the light vector and stay the same size whatever the camera does.
            s = kSpriteGhosts[i].x;
            radius = kSpriteGhosts[i].y;
        }

        const float2 center = light * s + jitter;
        if (proceduralMode) {
            radius *= lerp(0.55f, 1.0f, saturate(length(center) * 2.0f));
        }

        float2 local = pixel - center;
        // Odd blade counts flip the aperture image when the beam crosses after the stop; the
        // talk handles this by point-symmetrically flipping the shape at random rather than
        // tracking which side of the stop each ghost came from. The sprite table has no notion of
        // which side of the stop a ghost came from, so it never flips.
        if (proceduralMode && r1.y > 0.5f) {
            local = -local;
        }

        float d = SdRegularPolygon(local, radius, bladeCount);
        // circularity: blades are never perfectly straight and wide-open apertures are nearly
        // round, so the polygon is blended toward a circle.
        const float circleD = length(local) - radius;
        // A hand-authored sprite is usually just a soft disc -- the polygon comes from modelling the
        // diaphragm, which the sprite method does not do.
        const float circularity = proceduralMode ? lerp(0.15f, 0.75f, r0.x) : 1.0f;
        d = lerp(d, circleD, circularity);

        if (d > 0.0f) {
            continue;   // outside this ghost
        }

        // Normalised interior coordinate, 0 at the rim and 1 at the centre.
        const float fill = saturate(-d / max(radius, 0.0001f));

        // Vignetting. The talk's mask is e = d*b/(r - b) with centre Pe = P*r/(r - b), where b
        // is negative when the ghost is clipped from the outside -- and which side it is clipped
        // from is itself random. Barrel vignetting is why real ghosts thin out toward the frame
        // edge, so the mask is driven by how far the ghost sits from centre.
        const float vignetteOnset = 0.35f;
        const float clipFromOutside = (proceduralMode && Hash11(seed + 11.0f) > 0.5f) ? -1.0f : 1.0f;
        const float b = radius * clipFromOutside;
        const float rMask = 0.9f;
        const float maskRadius = abs(length(center) * b / max(rMask - b, 0.0001f));
        const float2 maskCenter = center * (rMask / max(rMask - b, 0.0001f));
        const float maskD = length(pixel - maskCenter) - max(maskRadius, 0.0001f);
        const float vignette = saturate(1.0f - smoothstep(vignetteOnset, 1.0f, length(center))) *
                               saturate(1.0f - smoothstep(-kEdgeSoftness, kEdgeSoftness, maskD * clipFromOutside));

        // Chromatic residue of the anti-reflective coating: a coating cancels reflection by
        // interference and what survives is coloured, so ghosts are tinted rather than white.
        // Two complementary colours interpolated across the distance field, normalised so the
        // pair sums to white, then modulated by the light's own colour.
        const float3 coatA = float3(0.35f, 0.55f, 1.00f);
        const float3 coatB = float3(1.00f, 0.60f, 0.25f);
        // The sprite table carries a flat authored tint per ghost instead of a coating model: the
        // colour is a choice, not a consequence of interference.
        float3 coat = proceduralMode
            ? lerp(coatA, coatB, saturate(fill * 1.3f + r0.y * 0.3f))
            : lerp(coatB, coatA, kSpriteGhosts[i].z);
        coat /= max((coat.r + coat.g + coat.b) / 3.0f, 0.0001f);

        // Soft rim, and brightness falling with area: a bigger ghost spreads the same energy.
        const float edge = saturate(-d / kEdgeSoftness);
        const float area = max(radius * radius, 0.0001f);
        // An AR coating leaves only a fraction of a percent per surface, and a ghost is at least
        // a double reflection, so these are dim. The first tuning pass used 0.0025 here and every
        // ghost clipped to flat white, which also destroyed the coating colour -- the tint only
        // survives while the result stays inside the display range.
        const float brightness = 0.00028f / area;

        // A real ghost is not a flat disc: it is the out-of-focus image of the aperture, brighter
        // toward the rim where the cone of rays piles up. Without this the polygons read as solid
        // stickers.
        const float profile = lerp(1.0f, 0.35f, saturate(fill * 1.4f));

        accumulated += coat * u_flareTint.rgb * (edge * vignette * brightness * profile);
    }

    return float4(accumulated * u_flareParams.z * visibility, 1.0f);
}
