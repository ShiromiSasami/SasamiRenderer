#ifndef SASAMI_RAY_OFFSET_HLSLI
#define SASAMI_RAY_OFFSET_HLSLI

// --------------------------------------------------------------------------
// Error-Bounded Ray Origin Offset
// Source: Waechter & Binder, "A Fast and Robust Method for Avoiding
//         Self-Intersection", Ray Tracing Gems ch.6 (2019).
//         https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/
//
// Offsets ray origin p along geometric normal n by an amount proportional to
// the floating-point ULP of each coordinate.  This guarantees the spawned ray
// never re-intersects the surface it was launched from, even for geometry that
// is far from the world origin (where a fixed bias would be too small) or very
// close to it (where manipulating ULPs alone would under-offset).
//
// Usage:  float3 safeOrigin = OffsetRay(hitPos, geometricNormal);
//         TraceClosestHit(safeOrigin, reflDir, 0.0f, tMax);
// --------------------------------------------------------------------------
float3 OffsetRay(float3 p, float3 n)
{
    static const float kOrigin     = 1.0f / 32.0f;   // linear region threshold
    static const float kFloatScale = 1.0f / 65536.0f; // linear region scale
    static const float kIntScale   = 256.0f;           // ULP nudge scale

    // Compute per-axis ULP offset as integer bit-manipulation
    int3 of_i = int3(kIntScale * n.x,
                     kIntScale * n.y,
                     kIntScale * n.z);

    // Add/subtract ULPs depending on sign of p (offset always away from surface)
    float3 p_i = float3(
        asfloat(asint(p.x) + (p.x < 0.0f ? -of_i.x : of_i.x)),
        asfloat(asint(p.y) + (p.y < 0.0f ? -of_i.y : of_i.y)),
        asfloat(asint(p.z) + (p.z < 0.0f ? -of_i.z : of_i.z)));

    // Near origin: ULP is too small, fall back to a linear offset
    return float3(
        abs(p.x) < kOrigin ? p.x + kFloatScale * n.x : p_i.x,
        abs(p.y) < kOrigin ? p.y + kFloatScale * n.y : p_i.y,
        abs(p.z) < kOrigin ? p.z + kFloatScale * n.z : p_i.z);
}

#endif // SASAMI_RAY_OFFSET_HLSLI
