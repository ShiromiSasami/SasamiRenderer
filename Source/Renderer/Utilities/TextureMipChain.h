#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace SasamiRenderer
{
    // CPU-side RGBA8 mip chain. Levels are stored back-to-back in `data`, tightly
    // packed (row pitch == width * 4), level 0 first.
    //
    // Scene textures were previously uploaded as a single level, so minified
    // surfaces sampled the full-resolution image and aliased badly (measured on
    // Bistro: a 0.17 degree camera rotation changed 16% of all pixels).
    struct TextureMipChain
    {
        struct Level
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::size_t byteOffset = 0; // offset of this level inside `data`
        };

        std::vector<std::uint8_t> data;
        std::vector<Level> levels;

        bool IsValid() const { return !levels.empty() && !data.empty(); }
        std::uint32_t LevelCount() const { return static_cast<std::uint32_t>(levels.size()); }
    };

    namespace TextureMipChainBuilder
    {
        // Builds a complete chain (down to 1x1) from a tightly packed RGBA8 top level.
        // Returns an empty chain when the input is invalid.
        //
        // srgbColor == true decodes RGB to linear, box-filters, and re-encodes; alpha is
        // always averaged directly. The raster path samples material textures as
        // R8G8B8A8_UNORM without an sRGB decode (see OpaqueGBuffer_PS.hlsl), so callers on
        // that path pass false to keep mip filtering consistent with the hardware's own
        // bilinear filtering of the same stored values.
        TextureMipChain BuildRgba8(const std::uint8_t* topLevelPixels,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   bool srgbColor);
    }
}
