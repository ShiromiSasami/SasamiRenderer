#pragma once

namespace SasamiEngine::Render::Voxel
{
    // Separating-axis triangle/AABB overlap test.
    // Reference: T. Akenine-Moeller, "Fast 3D Triangle-Box Overlap Testing", JGT 6(1), 2001.
    // The 13 candidate axes are the 3 box face normals, the triangle normal and the 9 cross
    // products of box edges with triangle edges. Touching (a separating distance of exactly
    // zero) counts as overlapping, so a triangle lying on a shared voxel face marks both voxels.
    // Degenerate (zero-area) triangles are tested against the face-normal axes and edge-cross
    // axes only; they never crash and never return true for a box they do not touch.
    bool TriangleOverlapsBox(const float boxCenter[3],
                             const float boxHalfSize[3],
                             const float v0[3],
                             const float v1[3],
                             const float v2[3]);
}

namespace SasamiEngine { using namespace Render; }
