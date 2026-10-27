#pragma once

#include "Renderer/Fluid/IsoSurfaceMesh.h"

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;
    struct VoxelGridDesc;

    // Zero level set of a voxel SDF (VoxelDistanceTransform.h, voxel units) as a world-space triangle mesh,
    // through the same Marching Tetrahedra as the particle iso-surfaces (Fluid::ExtractIsoSurface).
    // Samples sit on the voxel centres, so axis edges between an occupied and an empty voxel (-0.5 / +0.5)
    // cross on the shared voxel face; diagonal edges read the longer distances and round the corners.
    // Outside the grid the field counts as empty (+0.5), so a solid touching the border is capped there:
    // on the grid face where the border voxel reads -0.5, up to half a voxel beyond it deep inside.
    // The mesh is closed (no open edges) and its normals point out of the solid. Attributes are 0.
    // Empty or mismatched input gives an empty mesh.
    void ExtractSdfIsoSurface(const SignedDistanceGrid& field, const VoxelGridDesc& desc,
                              Fluid::IsoSurfaceMesh& outMesh);
}

namespace SasamiEngine { using namespace Render; }
