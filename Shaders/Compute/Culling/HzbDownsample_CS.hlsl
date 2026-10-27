//
// HzbDownsample_CS.hlsl
// One mip step of the reverse-Z HZB min pyramid. One thread per destination texel; the footprint
// follows the NPOT convention of Rasterization Grid (RasterGrid blog, 2010, "Hierarchical-Z map
// based occlusion culling"): dst = max(1, src / 2), and when the source dimension is odd the last
// destination texel also covers the extra source row/column. Mirrors HzbPyramid::Build on the CPU
// exactly (min is exact, so the result is bit-identical).
//
// Bindings (inline root descriptors, see GpuHzbBuilder):
//   t0 : g_params   single HzbLevelParams element (one dispatch per level)
//   u0 : g_pyramid  every level packed in one float buffer, offsets in floats
//

struct HzbLevelParams
{
    uint srcOffset;
    uint srcWidth;
    uint srcHeight;
    uint dstOffset;
    uint dstWidth;
    uint dstHeight;
    uint pad0;
    uint pad1;
};

StructuredBuffer<HzbLevelParams> g_params : register(t0);
RWStructuredBuffer<float> g_pyramid : register(u0);

[numthreads(8, 8, 1)]
void CSDownsample(uint3 dtid : SV_DispatchThreadID)
{
    const HzbLevelParams p = g_params[0];
    const uint x = dtid.x;
    const uint y = dtid.y;
    if (x >= p.dstWidth || y >= p.dstHeight)
    {
        return;
    }

    const uint xs = 2 * x;
    const uint ys = 2 * y;
    const uint xe = xs + (((p.srcWidth & 1u) != 0u && x == p.dstWidth - 1u) ? 2u : 1u);
    const uint ye = ys + (((p.srcHeight & 1u) != 0u && y == p.dstHeight - 1u) ? 2u : 1u);

    float result = g_pyramid[p.srcOffset + min(ys, p.srcHeight - 1u) * p.srcWidth + min(xs, p.srcWidth - 1u)];
    for (uint sy = ys; sy <= ye; ++sy)
    {
        const uint ry = min(sy, p.srcHeight - 1u);
        for (uint sx = xs; sx <= xe; ++sx)
        {
            const uint rx = min(sx, p.srcWidth - 1u);
            result = min(result, g_pyramid[p.srcOffset + ry * p.srcWidth + rx]);
        }
    }
    g_pyramid[p.dstOffset + y * p.dstWidth + x] = result;
}
