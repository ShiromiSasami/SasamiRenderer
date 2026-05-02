#ifndef SWRT_RESERVOIR_HLSLI
#define SWRT_RESERVOIR_HLSLI

// ---------------------------------------------------------------------------
// ReSTIR DI Reservoir
// One reservoir per pixel; stored in a RWStructuredBuffer<Reservoir>.
// ---------------------------------------------------------------------------

struct Reservoir
{
    uint  lightIndex;  // winning candidate (0xFFFFFFFF = invalid / no light)
    float wSum;        // accumulated importance weight sum (W_sum)
    uint  M;           // number of candidate samples seen so far
    float W;           // final resampling weight = wSum / (M * p_hat(y))
};

// Initialise an empty reservoir.
Reservoir InitReservoir()
{
    Reservoir r;
    r.lightIndex = 0xFFFFFFFFu;
    r.wSum       = 0.0f;
    r.M          = 0u;
    r.W          = 0.0f;
    return r;
}

// Weighted Reservoir Sampling update.
// x  : candidate light index
// w  : importance weight (p_hat(x))
// u  : uniform random in [0, 1)
void UpdateReservoir(inout Reservoir r, uint x, float w, float u)
{
    r.wSum += w;
    r.M++;
    if (u < w / max(r.wSum, 1e-30f))
        r.lightIndex = x;
}

// Combine (merge) a source reservoir into the destination.
// p_hat_src : evaluated target function at src.lightIndex in the dst pixel's domain.
// u         : uniform random in [0, 1).
void CombineReservoir(inout Reservoir dst, Reservoir src, float p_hat_src, float u)
{
    float wNew = p_hat_src * src.W * float(src.M);
    dst.wSum  += wNew;
    dst.M     += src.M;
    if (u < wNew / max(dst.wSum, 1e-30f))
        dst.lightIndex = src.lightIndex;
}

// Compute the final contribution weight W = wSum / (M * p_hat(y)).
// Call after all candidates have been processed.
void FinalizeReservoir(inout Reservoir r, float p_hat)
{
    r.W = (r.M > 0u && p_hat > 1e-30f)
        ? (r.wSum / (float(r.M) * p_hat))
        : 0.0f;
}

// ---------------------------------------------------------------------------
// Stateless PCG random number generator
// ---------------------------------------------------------------------------
uint PcgHash(uint v)
{
    uint s = v * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}

float Rand01(inout uint state)
{
    state = PcgHash(state);
    return float(state) * (1.0f / 4294967296.0f);
}

// Seed from pixel coord + frame index.
uint ReSTIRSeed(uint2 pixel, uint frameIndex, uint salt)
{
    return PcgHash(pixel.x ^ (pixel.y * 1619u) ^ (frameIndex * 2654435761u) ^ (salt * 805459861u));
}

// ---------------------------------------------------------------------------
// Shared p_hat (unnormalised target function) helpers
// Approximate luminance contribution of a point/spot light at a shading point.
// ---------------------------------------------------------------------------

float PhatPoint(float3 pos, float3 N, float3 lightPos, float3 lightColorIntensity, float lightRange)
{
    float3 toLight = lightPos - pos;
    float  dist    = length(toLight);
    if (dist >= lightRange || dist < 1e-5f) return 0.0f;
    float3 L       = toLight / dist;
    float  NdotL   = max(dot(N, L), 0.0f);
    float  t       = dist / max(lightRange, 1e-4f);
    float  atten   = saturate(1.0f - t * t) * (1.0f - t * t);
    float  lum     = dot(lightColorIntensity, float3(0.2126f, 0.7152f, 0.0722f));
    return lum * NdotL * atten;
}

#endif // SWRT_RESERVOIR_HLSLI
