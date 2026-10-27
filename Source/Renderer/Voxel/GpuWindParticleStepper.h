#pragma once

#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/WindDrivenParticles.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;
    class StaggeredVelocityGrid;

    // The HLSL StepParams of WindParticleStep_CS.hlsl (80 bytes).
    struct WindParticleStepParamsGpu
    {
        float origin[3];
        float voxelSize;
        float invVoxelSize;
        uint32_t dimX;
        uint32_t dimY;
        uint32_t dimZ;
        float dt;
        float tau;
        float decay;
        float buoyancy;
        float acceleration[3];
        float flutterAcceleration;
        float flutterFrequency;
        float wallFriction;
        uint32_t flutter;
        uint32_t particleCount;
    };
    static_assert(sizeof(WindParticleStepParamsGpu) == 80, "WindParticleStepParamsGpu must match the HLSL StepParams layout");

    // GPU twin of StepWindParticle (Shaders/Compute/Voxel/WindParticleStep_CS.hlsl): one thread per
    // particle, reading the SDF and the staggered (MAC) velocity of a ProjectedWindField from
    // structured float buffers. Only the steady projected field is supported (no gusts yet, 6c-7).
    //
    // 6c-1 validation path: Run submits, waits for the GPU and reads the particles back. The shader
    // is DXIL, so only the DX12 backend is supported for now and Initialize fails elsewhere.
    class GpuWindParticleStepper
    {
    public:
        ~GpuWindParticleStepper();

        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }

        // Copies the field into persistent GPU buffers (replacing any earlier upload). `sdf` is in
        // voxel units, on the same grid as `velocity` (ProjectedWindField::Grid()).
        bool UploadField(const SignedDistanceGrid& sdf, const StaggeredVelocityGrid& velocity,
                         const VoxelGridDesc& desc, std::string& outError);
        bool HasField() const { return m_sdfBuffer.IsValid(); }

        // Steps every particle `steps` times with the same params (one dispatch per step, UAV barrier
        // in between), then reads them back into `states`.
        bool Run(std::vector<WindParticleState>& states, const WindParticleStepParams& params, uint32_t steps,
                 std::string& outError);

        // StepParams for the uploaded field (origin, dims, voxel size) plus `params`.
        WindParticleStepParamsGpu MakeGpuParams(const WindParticleStepParams& params, uint32_t particleCount) const;

        // 6c-2: records one step into a caller's encoder (no submit, no wait) through inline root
        // descriptors: root 0-3 the field buffers, root 4 the StepParams at `paramsAddress` (an
        // upload-heap slot the caller keeps alive until the GPU is done), root 5 `particles` as a UAV.
        // `particles` must be in the UnorderedAccess state; a UAV barrier on it follows the dispatch.
        bool RecordStep(IRhiCommandEncoder& encoder, RhiGpuAddress paramsAddress, RhiBufferHandle particles,
                        uint32_t particleCount, std::string& outError) const;

        void ReleaseField();

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_bindableLayout{};
        RhiPipelineHandle m_bindablePipeline{};
        RhiPipelineLayoutHandle m_inlineLayout{};
        RhiPipelineHandle m_inlinePipeline{};
        // t0 sdf, t1 u, t2 v, t3 w, t4 params, u0 particles -- one descriptor per root table slot.
        RhiDescriptorAllocation m_descriptors{};

        VoxelGridDesc m_desc{};
        RhiBufferHandle m_sdfBuffer{};
        RhiBufferHandle m_uBuffer{};
        RhiBufferHandle m_vBuffer{};
        RhiBufferHandle m_wBuffer{};
        uint64_t m_sdfCount = 0;
        uint64_t m_uCount = 0;
        uint64_t m_vCount = 0;
        uint64_t m_wCount = 0;
    };
}
