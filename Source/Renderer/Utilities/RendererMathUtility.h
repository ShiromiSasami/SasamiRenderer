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

        void GenerateDiffuseShCoefficientsFromEquirect(const std::vector<float>& hdrPixels,
                                                       std::uint32_t hdrWidth,
                                                       std::uint32_t hdrHeight,
                                                       float outCoefficients[9][3]);

        void GeneratePrefilterCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                                  std::uint32_t hdrWidth,
                                                  std::uint32_t hdrHeight,
                                                  std::uint32_t baseFaceSize,
                                                  std::uint32_t mipLevels,
                                                  std::vector<std::vector<float>>& outSubresources);

        void GenerateBrdfLut(std::uint32_t width,
                             std::uint32_t height,
                             std::vector<float>& outPixels);

        // Generates a full mip chain for a skybox cubemap using box filtering.
        // baseFaces must have exactly 6 entries (face 0..5), each of size faceSize*faceSize*4 floats (RGBA).
        // mipLevels must be >= 1 and <= log2(faceSize)+1.
        // outSubresources will have mipLevels*6 entries ordered as [mip + face*mipLevels].
        void GenerateSkyboxCubemapMips(const std::vector<std::vector<float>>& baseFaces,
                                       std::uint32_t faceSize,
                                       std::uint32_t mipLevels,
                                       std::vector<std::vector<float>>& outSubresources);
    }
}
