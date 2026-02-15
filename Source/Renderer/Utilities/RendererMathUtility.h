#pragma once

#include <cstdint>
#include <vector>

namespace SasamiRenderer
{
    namespace RendererMathUtility
    {
        void GenerateSkyCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                            std::uint32_t hdrWidth,
                                            std::uint32_t hdrHeight,
                                            std::uint32_t faceSize,
                                            std::vector<std::vector<float>>& outFaces);

        void GenerateIrradianceCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                                   std::uint32_t hdrWidth,
                                                   std::uint32_t hdrHeight,
                                                   std::uint32_t faceSize,
                                                   std::vector<std::vector<float>>& outFaces);

        void GeneratePrefilterCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                                  std::uint32_t hdrWidth,
                                                  std::uint32_t hdrHeight,
                                                  std::uint32_t baseFaceSize,
                                                  std::uint32_t mipLevels,
                                                  std::vector<std::vector<float>>& outSubresources);

        void GenerateBrdfLut(std::uint32_t width,
                             std::uint32_t height,
                             std::vector<float>& outPixels);
    }
}
