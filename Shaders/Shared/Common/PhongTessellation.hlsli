#ifndef SASAMI_PHONG_TESSELLATION_HLSLI
#define SASAMI_PHONG_TESSELLATION_HLSLI

// Reference: Boubekeur & Alexa, "Phong Tessellation", SIGGRAPH Asia 2008.
float3 PhongProject(float3 p, float3 pv, float3 nv)
{
    return p - dot(p - pv, nv) * nv;
}

// Strength of the Phong smoothing in [0,1].
// 0 = flat linear (no change); 1 = full projection onto tangent planes.
static const float kPhongAlpha = 0.65f;

#endif // SASAMI_PHONG_TESSELLATION_HLSLI
