#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    // A leaf of the column quadtree: columns [x, x + size) x [z, z + size), clipped to the grid.
    struct QuadtreeLeaf
    {
        uint32_t x = 0;
        uint32_t z = 0;
        uint32_t size = 1;    // 1 << level
        uint32_t level = 0;
    };

    // Horizontal quadtree over the nx * nz columns of the FLIP grid (Quadtree Tall Cells, Narita et al.
    // 2025: the xz plane is refined by a quadtree, each leaf is one stack of cells). Every column lies in
    // exactly one leaf; edge-adjacent leaves differ by at most one level (2:1 balance).
    struct QuadtreeColumnLayout
    {
        static constexpr uint32_t MAX_LEVELS = 8;

        uint32_t nx = 0;
        uint32_t nz = 0;
        uint32_t maxLog2 = 0;
        std::vector<uint32_t> leafOf;          // nx * nz, x fastest: index into leaves
        std::vector<QuadtreeLeaf> leaves;      // ordered by (z, x) of their corner
        uint32_t leavesPerLevel[MAX_LEVELS] = {};

        uint32_t LeafAt(uint32_t x, uint32_t z) const { return leafOf[x + size_t(nx) * z]; }
    };

    // desiredLog2 (nx * nz) is the largest level allowed per column. The grid is tiled with
    // 2^maxLog2 blocks (clipped at the far edges); a leaf splits into four while any column inside it
    // wants a lower level than the leaf's, then leaves split until every edge neighbour is at most one
    // level apart. maxLog2 is clamped to MAX_LEVELS - 1. A missing or wrong-sized desiredLog2 means 0.
    QuadtreeColumnLayout BuildQuadtreeColumnLayout(uint32_t nx, uint32_t nz, const std::vector<uint8_t>& desiredLog2,
                                                   uint32_t maxLog2);

    // Checks coverage (every column in exactly one leaf, leafOf consistent), 2:1 balance and, when
    // desiredLog2 is given, level <= desired for every column. Returns false with a reason.
    bool ValidateQuadtreeColumnLayout(const QuadtreeColumnLayout& layout, const std::vector<uint8_t>* desiredLog2,
                                      std::string* reason);
}
