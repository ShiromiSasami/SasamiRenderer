#include "Renderer/Voxel/SdfIsoSurface.h"

#include "Renderer/Fluid/ParticleSplatGrid.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

#include <vector>

namespace SasamiEngine::Render::Voxel
{
    void ExtractSdfIsoSurface(const SignedDistanceGrid& field, const VoxelGridDesc& desc,
                              Fluid::IsoSurfaceMesh& outMesh)
    {
        outMesh.Clear();
        if (field.Values().empty() || field.DimX() != desc.dimX || field.DimY() != desc.dimY
            || field.DimZ() != desc.dimZ)
        {
            return;
        }

        // The extractor treats high values as inside, so feed it -d (occupied voxels are d <= -0.5).
        const std::vector<float>& values = field.Values();
        std::vector<float> density(values.size());
        for (size_t i = 0; i < values.size(); ++i)
        {
            density[i] = -values[i];
        }

        Fluid::SplatGridDesc splat;
        splat.origin[0] = desc.origin[0];
        splat.origin[1] = desc.origin[1];
        splat.origin[2] = desc.origin[2];
        splat.cellSize = desc.voxelSize;
        splat.dims[0] = desc.dimX;
        splat.dims[1] = desc.dimY;
        splat.dims[2] = desc.dimZ;
        Fluid::ExtractIsoSurface(splat, density.data(), 0.0f, -0.5f, outMesh);
    }
}
