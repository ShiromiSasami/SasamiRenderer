#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    namespace RendererMathUtility
    {
        void GenerateSkyCubemapFromEquirect(const std::vector<float>& hdrPixels,
                                            std::uint32_t hdrWidth,
                                            std::uint32_t hdrHeight,
                                            std::uint32_t faceSize,
                                            std::vector<std::vector<float>>& outFaces);

        void DownsampleFaceRgbaFloat(const std::vector<float>& src,
                                     std::uint32_t srcSize,
                                     std::vector<float>& outDst);

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
    }
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
