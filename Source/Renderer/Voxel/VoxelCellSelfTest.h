#pragma once

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // CPU-only checks of NibbleGrid and the two cell builders (VoxelCellAttributes.h):
    // nibble packing round-trip at word boundaries, coverage counts on hand-placed sub-voxels,
    // highest-class-wins, and the dimension-mismatch failures. Returns "OK ..." or "ERR ...".
    std::string RunVoxelCellSelfTest();
}

namespace SasamiEngine { using namespace Render; }
