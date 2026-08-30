// Downsamples one cubemap mip level into the next with a 2x2 box filter.
//
// Dispatched once per destination mip, over all 6 faces at once (z = face).
// Faces are filtered independently, matching the CPU mip builder: no cross-face
// tap at the cube edges. The source is bound as a single-mip SRV (an SRV with
// MostDetailedMip set to the parent level), so the Load below uses mip 0.
//
// Source coordinates are clamped rather than assumed to be exactly 2x the
// destination, so non-power-of-two and 1-texel levels stay in bounds.

cbuffer SkyDownsampleCB : register(b0)
{
    uint2 g_dstSize;
    uint2 g_srcSize;
};

Texture2DArray<float4>   g_srcMip : register(t0);
RWTexture2DArray<float4> g_dstMip : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_dstSize.x || tid.y >= g_dstSize.y) {
        return;
    }

    const uint face = tid.z;
    const uint2 maxSrc = g_srcSize - uint2(1, 1);
    const uint2 s0 = min(tid.xy * 2u, maxSrc);
    const uint2 s1 = min(tid.xy * 2u + uint2(1, 1), maxSrc);

    const float4 c00 = g_srcMip.Load(int4(s0.x, s0.y, face, 0));
    const float4 c10 = g_srcMip.Load(int4(s1.x, s0.y, face, 0));
    const float4 c01 = g_srcMip.Load(int4(s0.x, s1.y, face, 0));
    const float4 c11 = g_srcMip.Load(int4(s1.x, s1.y, face, 0));

    g_dstMip[uint3(tid.xy, face)] = (c00 + c10 + c01 + c11) * 0.25f;
}
