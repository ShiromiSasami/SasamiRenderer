#include "Renderer/Voxel/ParityLeakCount.h"

namespace SasamiEngine::Render::Voxel
{
    uint64_t CountParityLeakRows(const OccupancyBitGrid& interior)
    {
        if (interior.DimX() == 0)
        {
            return 0;
        }

        const uint32_t lastX = interior.DimX() - 1;
        uint64_t leakRows = 0;
        for (uint32_t z = 0; z < interior.DimZ(); ++z)
        {
            for (uint32_t y = 0; y < interior.DimY(); ++y)
            {
                if (interior.Test(lastX, y, z))
                {
                    ++leakRows;
                }
            }
        }
        return leakRows;
    }
}
