#pragma once

#include "Renderer/RHI/RhiDevice.h"

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // Voxelizes fixed test meshes with both the CPU reference (VoxelizeSurface) and
    // GpuSurfaceVoxelizer and compares the two grids word for word. Blocks until the GPU is
    // idle, so call it between frames on the main thread. Returns a one-line report starting
    // with "OK" when every case matches bit for bit, "ERR" otherwise.
    std::string RunVoxelizerSelfTest(IRhiDevice& device);
}

namespace SasamiEngine { using namespace Render; }
