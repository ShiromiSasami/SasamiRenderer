#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Reads an R32F 3D texture back through a compute pass (Shaders/Compute/Voxel/SdfReadback_CS.hlsl):
    // one thread per texel Loads it and writes it to a structured float buffer in x + y*w + z*w*h
    // order, which is then copied to a readback buffer. The RHI has no texture-to-buffer copy, so this
    // is how `voxel.sdf check` proves that CreateRhiTexture3DFromData stored the SDF bit for bit.
    //
    // One-shot validation path (submit + wait). The shader is DXIL, so only the DX12 backend is
    // supported for now and Initialize fails elsewhere.
    class GpuSdfTextureReadback
    {
    public:
        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }

        // `texture` must be an R32F 3D texture of the given size, in the shader-resource state.
        bool Readback(RhiTextureHandle texture, uint32_t width, uint32_t height, uint32_t depth,
                      std::vector<float>& outTexels, std::string& outError);

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_bindableLayout{};
        RhiPipelineHandle m_bindablePipeline{};
        // t0 texture, t1 params, u0 output -- one descriptor per root table slot.
        RhiDescriptorAllocation m_descriptors{};
    };
}
