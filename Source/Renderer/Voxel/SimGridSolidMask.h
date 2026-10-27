#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // Smoke simulation grid. Cell (i,j,k) is the axis-aligned box of side `cellSize` CENTRED at
    // origin + (i,j,k) * cellSize; the centre is not offset by half a cell. This matches
    // SmokeVolume_CS.hlsl, where worldPos = origin + id * cellSize.
    struct SimGridDesc
    {
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float cellSize = 1.0f;
        uint32_t countX = 0;
        uint32_t countY = 0;
        uint32_t countZ = 0;
    };

    // Resamples the scene's 1-bit voxel occupancy onto the simulation's cell grid. The result is
    // the solid boundary for SmokeVolumeSim: the GPU shader reads it as StructuredBuffer<uint>.
    // Obstacles are voxelized into an inside/outside grid that the solver reads, as in Crane,
    // Llamas & Tariq, "Real-Time Simulation and Rendering of 3D Fluids", GPU Gems 3 ch.30 (2007).
    // CPU only and backend-neutral: no GPU API is touched here.
    //
    // A cell is solid when ANY occupied voxel overlaps its box with positive volume (open-interval
    // test on every axis, voxMin < cellMax && voxMax > cellMin), so it is conservative and thin
    // walls survive when the voxels are smaller than the cells. Voxel v on an axis covers
    // [origin + v * voxelSize, origin + (v + 1) * voxelSize).
    //
    // outWords is resized to (countX * countY * countZ + 31) / 32 and zero-filled. Cell bit index
    // is x + y * countX + z * countX * countY, stored at word idx >> 5, bit idx & 31 (LSB first).
    // Returns the number of solid cells. An empty sim grid (any count 0) yields empty words and 0;
    // so does an occupancy grid whose dimensions differ from voxelDesc (words stay all zero).
    uint64_t BuildSimGridSolidMask(const VoxelGridDesc& voxelDesc,
                                   const OccupancyBitGrid& occupancy,
                                   const SimGridDesc& sim,
                                   std::vector<uint32_t>& outWords);
}

namespace SasamiEngine { using namespace Render; }
