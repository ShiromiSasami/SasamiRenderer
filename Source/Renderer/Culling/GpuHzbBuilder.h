#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Culling
{
    // One level of a GPU HZB packed into a single float buffer.
    struct GpuHzbLevel
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t offset = 0; // in floats from the start of the pyramid buffer
    };

    // GPU twin of HzbPyramid: builds the same reverse-Z min pyramid with one compute dispatch per
    // level (Shaders/Compute/Culling/HzbDownsample_CS.hlsl). Every level lives in one structured
    // float buffer, so the result must be bit-identical to HzbPyramid::Build (min is exact).
    // Re-check with `hzb gpu`.
    //
    // DX12-only for now (root descriptors need GetRhiBufferGpuAddress), like CrowdGpuCuller.
    class GpuHzbBuilder
    {
    public:
        // Same NPOT floor convention as HzbPyramid: dim = max(1, prev / 2) until 1x1.
        static std::vector<GpuHzbLevel> ComputeLayout(uint32_t width, uint32_t height, uint32_t& outTotalFloats);

        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }
        void Shutdown();

        // One-shot: uploads `depth` as level 0, records every downsample, submits, waits for the GPU
        // and reads the whole pyramid back into `outTexels` (laid out as ComputeLayout says).
        bool BuildAndReadback(const float* depth, uint32_t width, uint32_t height,
                              std::vector<float>& outTexels, std::vector<GpuHzbLevel>& outLevels,
                              std::string& outError);

        ~GpuHzbBuilder() { Shutdown(); }

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_layout{};
        RhiPipelineHandle m_pipeline{};
    };
}
