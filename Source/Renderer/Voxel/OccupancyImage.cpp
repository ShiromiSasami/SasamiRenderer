#include "Renderer/Voxel/OccupancyImage.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        void ImageSize(const OccupancyBitGrid& grid, VoxelAxis axis, uint32_t& width, uint32_t& height)
        {
            switch (axis)
            {
            case VoxelAxis::X:
                width = grid.DimZ();
                height = grid.DimY();
                break;
            case VoxelAxis::Y:
                width = grid.DimX();
                height = grid.DimZ();
                break;
            case VoxelAxis::Z:
            default:
                width = grid.DimX();
                height = grid.DimY();
                break;
            }
        }

        bool IsSet(const std::vector<uint32_t>& words, uint64_t linear)
        {
            return ((words[static_cast<size_t>(linear >> 5)] >> (linear & 31u)) & 1u) != 0u;
        }

        std::vector<uint8_t> BuildSlice(const OccupancyBitGrid& grid, VoxelAxis axis, uint32_t layer,
                                        uint32_t srcW, uint32_t srcH)
        {
            const std::vector<uint32_t>& words = grid.Words();
            const uint64_t dimX = grid.DimX();
            const uint64_t dimY = grid.DimY();
            std::vector<uint8_t> out(static_cast<size_t>(srcW) * srcH, 0);

            for (uint32_t v = 0; v < srcH; ++v)
            {
                for (uint32_t u = 0; u < srcW; ++u)
                {
                    uint64_t x = 0;
                    uint64_t y = 0;
                    uint64_t z = 0;
                    switch (axis)
                    {
                    case VoxelAxis::X:
                        x = layer;
                        y = dimY - 1 - v;
                        z = u;
                        break;
                    case VoxelAxis::Y:
                        x = u;
                        y = layer;
                        z = v;
                        break;
                    case VoxelAxis::Z:
                    default:
                        x = u;
                        y = dimY - 1 - v;
                        z = layer;
                        break;
                    }
                    const uint64_t linear = x + y * dimX + z * dimX * dimY;
                    if (IsSet(words, linear))
                    {
                        out[static_cast<size_t>(v) * srcW + u] = 255;
                    }
                }
            }
            return out;
        }

        std::vector<uint8_t> BuildProjection(const OccupancyBitGrid& grid, VoxelAxis axis,
                                             uint32_t srcW, uint32_t srcH)
        {
            const std::vector<uint32_t>& words = grid.Words();
            const uint32_t dimX = grid.DimX();
            const uint32_t dimY = grid.DimY();
            const uint32_t dimZ = grid.DimZ();
            std::vector<uint32_t> counts(static_cast<size_t>(srcW) * srcH, 0);

            // Memory order: z outer, y, x inner. Along a row the target pixel either stays put (axis X)
            // or advances with x (axes Y and Z).
            const size_t xStep = (axis == VoxelAxis::X) ? 0u : 1u;

            for (uint32_t z = 0; z < dimZ; ++z)
            {
                for (uint32_t y = 0; y < dimY; ++y)
                {
                    size_t pixelBase = 0;
                    switch (axis)
                    {
                    case VoxelAxis::X:
                        pixelBase = static_cast<size_t>(dimY - 1 - y) * srcW + z;
                        break;
                    case VoxelAxis::Y:
                        pixelBase = static_cast<size_t>(z) * srcW;
                        break;
                    case VoxelAxis::Z:
                    default:
                        pixelBase = static_cast<size_t>(dimY - 1 - y) * srcW;
                        break;
                    }

                    uint64_t linear = (static_cast<uint64_t>(z) * dimY + y) * dimX;
                    uint32_t x = 0;
                    while (x < dimX)
                    {
                        const uint32_t word = words[static_cast<size_t>(linear >> 5)];
                        const uint32_t bitOffset = static_cast<uint32_t>(linear & 31u);
                        if (bitOffset == 0 && word == 0 && x + 32 <= dimX)
                        {
                            x += 32;
                            linear += 32;
                            continue;
                        }
                        if (((word >> bitOffset) & 1u) != 0u)
                        {
                            ++counts[pixelBase + xStep * x];
                        }
                        ++x;
                        ++linear;
                    }
                }
            }

            uint32_t maxCount = 0;
            for (const uint32_t c : counts)
            {
                maxCount = std::max(maxCount, c);
            }

            std::vector<uint8_t> out(counts.size(), 0);
            if (maxCount == 0)
            {
                return out;
            }
            for (size_t i = 0; i < counts.size(); ++i)
            {
                const uint64_t c = counts[i];
                if (c == 0)
                {
                    continue;
                }
                const uint64_t scaled = (c * 255u * 2u + maxCount) / (2u * static_cast<uint64_t>(maxCount));
                out[i] = static_cast<uint8_t>(std::clamp<uint64_t>(scaled, 1u, 255u));
            }
            return out;
        }
    }

    uint32_t AxisDim(const OccupancyBitGrid& grid, VoxelAxis axis)
    {
        switch (axis)
        {
        case VoxelAxis::X: return grid.DimX();
        case VoxelAxis::Y: return grid.DimY();
        case VoxelAxis::Z:
        default: return grid.DimZ();
        }
    }

    OccupancyImage BuildOccupancyImage(const OccupancyBitGrid& grid,
                                       VoxelAxis axis,
                                       OccupancyImageMode mode,
                                       uint32_t sliceIndex,
                                       uint32_t maxSide)
    {
        OccupancyImage image;
        if (grid.DimX() == 0 || grid.DimY() == 0 || grid.DimZ() == 0)
        {
            return image;
        }

        uint32_t srcW = 0;
        uint32_t srcH = 0;
        ImageSize(grid, axis, srcW, srcH);

        std::vector<uint8_t> source;
        if (mode == OccupancyImageMode::Slice)
        {
            const uint32_t layer = std::min(sliceIndex, AxisDim(grid, axis) - 1);
            source = BuildSlice(grid, axis, layer, srcW, srcH);
        }
        else
        {
            source = BuildProjection(grid, axis, srcW, srcH);
        }

        uint32_t factor = 1;
        if (maxSide > 0 && (srcW > maxSide || srcH > maxSide))
        {
            const uint32_t factorW = (srcW + maxSide - 1) / maxSide;
            const uint32_t factorH = (srcH + maxSide - 1) / maxSide;
            factor = std::max(factorW, factorH);
        }

        image.sourceWidth = srcW;
        image.sourceHeight = srcH;
        image.downsample = factor;
        image.width = (srcW + factor - 1) / factor;
        image.height = (srcH + factor - 1) / factor;

        if (factor == 1)
        {
            image.values = std::move(source);
            return image;
        }

        image.values.assign(static_cast<size_t>(image.width) * image.height, 0);
        for (uint32_t oy = 0; oy < image.height; ++oy)
        {
            const uint32_t y0 = oy * factor;
            const uint32_t y1 = std::min(y0 + factor, srcH);
            for (uint32_t ox = 0; ox < image.width; ++ox)
            {
                const uint32_t x0 = ox * factor;
                const uint32_t x1 = std::min(x0 + factor, srcW);
                uint8_t block = 0;
                for (uint32_t sy = y0; sy < y1; ++sy)
                {
                    const uint8_t* row = source.data() + static_cast<size_t>(sy) * srcW;
                    for (uint32_t sx = x0; sx < x1; ++sx)
                    {
                        block = std::max(block, row[sx]);
                    }
                }
                image.values[static_cast<size_t>(oy) * image.width + ox] = block;
            }
        }
        return image;
    }
}
