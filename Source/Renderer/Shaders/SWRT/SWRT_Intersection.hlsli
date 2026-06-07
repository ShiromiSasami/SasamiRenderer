#ifndef SASAMI_SWRT_INTERSECTION_HLSLI
#define SASAMI_SWRT_INTERSECTION_HLSLI

// Shared primitive intersection and transform helpers.

bool IntersectRayAabb(float3 ro, float3 rinvd,
                      float3 bMin, float3 bMax,
                      float tMax, out float tEntry)
{
    float3 t1 = (bMin - ro) * rinvd;
    float3 t2 = (bMax - ro) * rinvd;
    float3 tlo = min(t1, t2);
    float3 thi = max(t1, t2);
    tEntry = max(tlo.x, max(tlo.y, tlo.z));
    float tExit  = min(thi.x, min(thi.y, min(thi.z, tMax)));
    return tExit >= tEntry;
}

// --------------------------------------------------------------------------
// Mﾃｶller窶典rumbore triangle intersection
// Returns true if ray hits the triangle, setting t and barycentrics (u,v).
// --------------------------------------------------------------------------
bool IntersectRayTriangle(float3 ro, float3 rd,
                          float3 p0, float3 edge1, float3 edge2,
                          out float t, out float u, out float v)
{
    float3 h = cross(rd, edge2);
    float  a = dot(edge1, h);
    if (abs(a) < 1e-7f)
    {
        t = 0.0f; u = 0.0f; v = 0.0f;
        return false;
    }
    float  inv_a = 1.0f / a;
    float3 s = ro - p0;
    u = dot(s, h) * inv_a;
    if (u < 0.0f || u > 1.0f)
    {
        t = 0.0f; v = 0.0f;
        return false;
    }
    float3 q = cross(s, edge1);
    v = dot(rd, q) * inv_a;
    if (v < 0.0f || u + v > 1.0f)
    {
        t = 0.0f;
        return false;
    }
    t = dot(edge2, q) * inv_a;
    return t > 1e-6f;
}

// --------------------------------------------------------------------------
// Transform a point / vector by a float4x4 stored in a StructuredBuffer.
//
// GpuInstanceInfo.world / invWorld are C++ float[16] arrays in row-major
// (row-vector) convention, copied with memcpy into a StructuredBuffer whose
// float4x4 fields are column-major by default in HLSL.  HLSL therefore reads
// the raw memory as the TRANSPOSE of the intended matrix, which is exactly the
// column-vector-convention representation of the same transform.
//
// Consequence: to apply the transform correctly we must use mul(M, v) 窶・// i.e., matrix on the LEFT 窶・not mul(v, M).
//
//   mul(m_colmaj, float4(p,1)) == M_original * p  (column-vector application)
//
// Using mul(float4(p,1), m) would compute M^T * p 窶・i.e., the wrong transform.
// --------------------------------------------------------------------------
float3 TransformPoint4x4(float4x4 m, float3 p)
{
    float4 r = mul(m, float4(p, 1.0f));
    return r.xyz / r.w;
}

float3 TransformVector4x4(float4x4 m, float3 v)
{
    return mul(m, float4(v, 0.0f)).xyz;
}

#endif // SASAMI_SWRT_INTERSECTION_HLSLI
