#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // Compares GpuSolidVoxelizer with VoxelizeSolidParity bit for bit on meshes chosen to hit
    // the tie rules: a random triangle soup, vertices on the half-voxel lattice, a box whose
    // corners sit on voxel centres, and one triangle spanning the whole grid, with dimX not a
    // multiple of 32. Parity is compared as is, so the meshes need not be closed. Blocks until
    // the GPU is idle. Returns a one-line report starting with "OK" or "ERR".
    std::string RunGpuSolidVoxelizerSelfTest(IRhiDevice& device);
}

namespace SasamiEngine { using namespace Render; }
