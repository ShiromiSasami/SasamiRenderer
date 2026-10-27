#pragma once

#include "Renderer/Voxel/OccupancyBitGrid.h"

#include <string>

namespace SasamiEngine::Render::Voxel
{
    // Round-trips `dense` through BrickOccupancyGrid and compares every voxel. Appends a
    // "dense=..B sparse=..B ratio=.. full/mixed/empty bricks diffBits=.." summary to `out` and
    // returns true when the decoded grid matches word for word.
    bool CheckBrickRoundTrip(const OccupancyBitGrid& dense, std::string& out);

    // Fixed synthetic cases (empty, full, sparse random, a filled sphere) on grid sizes that
    // are not multiples of the brick side. CPU only. Returns a one-line report starting with
    // "OK" when every case round-trips exactly, "ERR" otherwise.
    std::string RunBrickOccupancySelfTest();
}

namespace SasamiEngine { using namespace Render; }
