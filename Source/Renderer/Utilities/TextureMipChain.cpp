#include "Renderer/Utilities/TextureMipChain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace SasamiRenderer
{
    namespace
    {
        float SrgbToLinear(float c)
        {
            return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c)
        {
            return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        }

        // 8bit sRGB -> linear lookup table, built once via a thread-safe local static.
        const std::array<float, 256>& SrgbToLinearTable()
        {
            static const std::array<float, 256> table = [] {
                std::array<float, 256> t{};
                for (int i = 0; i < 256; ++i) {
                    t[static_cast<std::size_t>(i)] = SrgbToLinear(static_cast<float>(i) / 255.0f);
                }
                return t;
            }();
            return table;
        }

        std::uint8_t QuantizeSrgb(float linear)
        {
            const float srgb = LinearToSrgb(std::clamp(linear, 0.0f, 1.0f));
            const int quantized = static_cast<int>(std::lround(srgb * 255.0f));
            return static_cast<std::uint8_t>(std::clamp(quantized, 0, 255));
        }

        std::uint8_t AverageChannel(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
        {
            return static_cast<std::uint8_t>((static_cast<int>(a) + b + c + d + 2) / 4);
        }

        std::uint32_t ComputeLevelCount(std::uint32_t width, std::uint32_t height)
        {
            std::uint32_t levelCount = 1;
            std::uint32_t maxDim = std::max(width, height);
            while (maxDim > 1) {
                maxDim >>= 1;
                ++levelCount;
            }
            return levelCount;
        }
    }

    namespace TextureMipChainBuilder
    {
        TextureMipChain BuildRgba8(const std::uint8_t* topLevelPixels,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   bool srgbColor)
        {
            TextureMipChain chain;
            if (!topLevelPixels || width == 0 || height == 0) {
                return chain;
            }

            const std::uint32_t levelCount = ComputeLevelCount(width, height);
            chain.levels.reserve(levelCount);

            std::size_t totalBytes = 0;
            std::uint32_t levelWidth = width;
            std::uint32_t levelHeight = height;
            for (std::uint32_t i = 0; i < levelCount; ++i) {
                TextureMipChain::Level level;
                level.width = levelWidth;
                level.height = levelHeight;
                level.byteOffset = totalBytes;
                chain.levels.push_back(level);

                totalBytes += static_cast<std::size_t>(levelWidth) * levelHeight * 4u;
                levelWidth = std::max(1u, levelWidth / 2u);
                levelHeight = std::max(1u, levelHeight / 2u);
            }

            chain.data.resize(totalBytes);

            const std::size_t level0Bytes = static_cast<std::size_t>(width) * height * 4u;
            std::memcpy(chain.data.data(), topLevelPixels, level0Bytes);

            const std::array<float, 256>& srgbTable = SrgbToLinearTable();

            for (std::uint32_t i = 1; i < levelCount; ++i) {
                const TextureMipChain::Level& parentLevel = chain.levels[i - 1];
                const TextureMipChain::Level& dstLevel = chain.levels[i];
                const std::uint8_t* parentPixels = chain.data.data() + parentLevel.byteOffset;
                std::uint8_t* dstPixels = chain.data.data() + dstLevel.byteOffset;

                for (std::uint32_t y = 0; y < dstLevel.height; ++y) {
                    const std::uint32_t y0 = y * 2u;
                    const std::uint32_t y1 = std::min(y0 + 1u, parentLevel.height - 1u);
                    for (std::uint32_t x = 0; x < dstLevel.width; ++x) {
                        const std::uint32_t x0 = x * 2u;
                        const std::uint32_t x1 = std::min(x0 + 1u, parentLevel.width - 1u);

                        const std::uint8_t* p00 = parentPixels + (static_cast<std::size_t>(y0) * parentLevel.width + x0) * 4u;
                        const std::uint8_t* p01 = parentPixels + (static_cast<std::size_t>(y0) * parentLevel.width + x1) * 4u;
                        const std::uint8_t* p10 = parentPixels + (static_cast<std::size_t>(y1) * parentLevel.width + x0) * 4u;
                        const std::uint8_t* p11 = parentPixels + (static_cast<std::size_t>(y1) * parentLevel.width + x1) * 4u;

                        std::uint8_t* dst = dstPixels + (static_cast<std::size_t>(y) * dstLevel.width + x) * 4u;

                        for (int channel = 0; channel < 3; ++channel) {
                            if (srgbColor) {
                                const float sum = srgbTable[p00[channel]] + srgbTable[p01[channel]] +
                                                  srgbTable[p10[channel]] + srgbTable[p11[channel]];
                                dst[channel] = QuantizeSrgb(sum * 0.25f);
                            } else {
                                dst[channel] = AverageChannel(p00[channel], p01[channel], p10[channel], p11[channel]);
                            }
                        }
                        dst[3] = AverageChannel(p00[3], p01[3], p10[3], p11[3]);
                    }
                }
            }

            return chain;
        }
    }
}
