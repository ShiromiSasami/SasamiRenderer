#pragma once

#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/VoxelRegion.h"

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // GPU counterpart of VoxelizeSurface (CpuSurfaceVoxelizer.h): one thread per triangle tests
    // every voxel in the triangle's clamped AABB with the same 13-axis SAT as
    // TriangleOverlapsBox and ORs the hits into a dense 1-bit grid whose layout is
    // OccupancyBitGrid's. This is the triangle-parallel variant of conservative surface
    // voxelization (Schwarz & Seidel 2010, sec. 3); Shaders/Compute/Voxel/SurfaceVoxelize_CS.hlsl.
    //
    // Currently a one-shot path: Voxelize uploads the mesh, dispatches, waits for the GPU and
    // reads the grid back. It is meant for validation against the CPU reference, not per frame.
    // Built only through IRhiDevice / IRhiCommandEncoder; the shader is compiled to DXIL, so only
    // the DX12 backend is supported for now and Initialize fails elsewhere.
    //
    // Known limit: a thread walks its triangle's whole AABB, so one huge triangle in a fine grid
    // costs O(AABB volume) on a single thread.
    class GpuSurfaceVoxelizer
    {
    public:
        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }

        // Overwrites outGrid (resized to desc's dimensions) with the GPU result.
        // indices == nullptr is handled as in VoxelizeSurface (consecutive position triples).
        bool Voxelize(const VoxelGridDesc& desc,
                      const VoxelizerMeshInput& mesh,
                      OccupancyBitGrid& outGrid,
                      std::string& outError);

        // Voxelize restricted to `region` (VoxelRegion.h): outGrid is resized to region.size and
        // receives voxel (region.min + local) at local. The shader places voxels with desc's
        // full-grid origin, so every bit equals Voxelize's bit at that voxel, and equals
        // VoxelizeSurfaceRegion's. Voxelize is this with FullVoxelRegion(desc). Triangles whose
        // clamped range misses the region exit early. Fails on an empty or out-of-grid region.
        bool VoxelizeRegion(const VoxelGridDesc& desc,
                            const VoxelRegion& region,
                            const VoxelizerMeshInput& mesh,
                            OccupancyBitGrid& outGrid,
                            std::string& outError);

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_bindableLayout{};
        RhiPipelineHandle m_bindablePipeline{};
        // t0 positions, t1 indices, t2 params, u0 bits -- one descriptor per root table slot.
        RhiDescriptorAllocation m_descriptors{};
    };
}

namespace SasamiEngine { using namespace Render; }
