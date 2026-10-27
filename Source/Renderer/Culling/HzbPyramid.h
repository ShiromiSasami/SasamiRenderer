#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Culling
{
    // Inclusive level-0 texel bounds of a screen rect.
    struct HzbRect
    {
        uint32_t x0 = 0;
        uint32_t y0 = 0;
        uint32_t x1 = 0;
        uint32_t y1 = 0;
    };

    // CPU reference Hierarchical-Z pyramid; the bit-exact reference for the GPU crowd HZB cull.
    // The engine is reverse-Z (1 = near, 0 = far), so every level stores the MIN depth (farthest)
    // of its footprint. An object is occluded when its closest depth (max depth value of its
    // bounds) is strictly less than the HZB min over its footprint.
    //
    // References:
    //  - RasterGrid, "Hierarchical-Z map based occlusion culling" (2010): non-power-of-two
    //    pyramid, dim = max(1, floor(prev / 2)), odd edge extended by one extra texel.
    //  - Niessner and Loop, "Patch-Based Occlusion Culling for Hardware Tessellation" (2012):
    //    pick the level where the rect fits in 2x2 texels.
    class HzbPyramid
    {
    public:
        // Level 0 is a copy of `depth` (row-major, width * height floats).
        void Build(const float* depth, uint32_t width, uint32_t height);

        uint32_t LevelCount() const { return static_cast<uint32_t>(m_levels.size()); }
        uint32_t LevelWidth(uint32_t level) const { return m_levels[level].width; }
        uint32_t LevelHeight(uint32_t level) const { return m_levels[level].height; }
        float Texel(uint32_t level, uint32_t x, uint32_t y) const { return m_levels[level].texels[y * m_levels[level].width + x]; }
        const float* LevelData(uint32_t level) const { return m_levels[level].texels.data(); }
        size_t ByteSize() const;

        // Min depth over a conservative texel set covering `rect`. Result <= true level-0 min.
        float FootprintMin(const HzbRect& rect, uint32_t* outLevel = nullptr) const;

        // True only when the whole rect is provably in front of nothing closer than the HZB.
        bool IsOccluded(const HzbRect& rect, float closestDepth) const;

        // u, v in [0,1]; texel = floor(u * size) clamped to size - 1.
        static HzbRect RectFromUv(float u0, float v0, float u1, float v1, uint32_t width, uint32_t height);

    private:
        struct Level
        {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<float> texels;
        };

        std::vector<Level> m_levels;
    };
}
