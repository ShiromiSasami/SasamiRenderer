#pragma once

#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // GPU counterpart of VoxelizeSolidParity (CpuSolidVoxelizer.h), bit-identical to it.
    // Schwarz & Seidel 2010, sec. 4, in two compute passes over a row-padded bit grid
    // (Shaders/Compute/Voxel/SolidVoxelize_CS.hlsl):
    //   1. CSCrossings, one thread per triangle: for every voxel row the triangle covers, XOR
    //      a single bit at the first voxel past the crossing (InterlockedXor).
    //   2. CSPrefix, one thread per row: prefix XOR along +X, which turns each single bit into
    //      the flipped row tail the CPU reference writes.
    // Each row owns ceil(dimX / 32) words on the GPU so the prefix never crosses rows; Voxelize
    // repacks into OccupancyBitGrid's dense layout on readback.
    //
    // The coverage and crossing tests run in double with only add, multiply and compare (the
    // base D3D11 double ops, no division or fma), in the same order as the CPU reference, so
    // the results match bit for bit. The device must support double-precision shader ops;
    // otherwise pipeline creation fails and Initialize reports it.
    //
    // DX12 backend only (the shader is compiled to DXIL); Initialize fails elsewhere. Voxelize
    // uploads, dispatches, waits for the GPU and reads back: validation use, not per frame.
    // Like GpuSurfaceVoxelizer, one huge triangle in a fine grid runs on a single thread.
    class GpuSolidVoxelizer
    {
    public:
        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }

        // Same contract as VoxelizeSolidParity except that outGrid is reset to desc's
        // dimensions and overwritten, not XOR-ed into.
        bool Voxelize(const VoxelGridDesc& desc,
                      const VoxelizerMeshInput& mesh,
                      OccupancyBitGrid& outGrid,
                      std::string& outError);

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_bindableLayout{};
        RhiPipelineHandle m_bindableCrossingsPipeline{};
        RhiPipelineHandle m_bindablePrefixPipeline{};
        // t0 positions, t1 indices, t2 params, u0 bits.
        RhiDescriptorAllocation m_descriptors{};
    };
}

namespace SasamiEngine { using namespace Render; }
