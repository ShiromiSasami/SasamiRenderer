#include "Renderer/Voxel/SdfIsoSurfaceSelfTest.h"

#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SdfIsoSurface.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include <cmath>
#include <sstream>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float PI = 3.14159265358979f;

        VoxelGridDesc MakeDesc(uint32_t dimX, uint32_t dimY, uint32_t dimZ, float voxelSize)
        {
            VoxelGridDesc desc;
            desc.origin[0] = 1.0f;
            desc.origin[1] = -2.0f;
            desc.origin[2] = 3.0f;
            desc.voxelSize = voxelSize;
            desc.dimX = dimX;
            desc.dimY = dimY;
            desc.dimZ = dimZ;
            return desc;
        }

        // True when every vertex lies inside the grid box grown by `margin` voxels.
        bool VerticesInsideGrownGrid(const Fluid::IsoSurfaceMesh& mesh, const VoxelGridDesc& desc, float margin)
        {
            const uint32_t dims[3] = { desc.dimX, desc.dimY, desc.dimZ };
            const float eps = 1e-4f * desc.voxelSize;
            for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
            {
                for (int a = 0; a < 3; ++a)
                {
                    const float lo = desc.origin[a] - margin * desc.voxelSize - eps;
                    const float hi = desc.origin[a] + (float(dims[a]) + margin) * desc.voxelSize + eps;
                    if (mesh.positions[i + a] < lo || mesh.positions[i + a] > hi)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    std::string RunSdfIsoSurfaceSelfTest()
    {
        uint32_t checks = 0;
        std::string firstError;
        auto check = [&](bool condition, const std::string& description)
        {
            ++checks;
            if (!condition && firstError.empty())
            {
                firstError = description;
            }
        };

        std::ostringstream info;

        // (a) Voxelized sphere, radius 7 voxels, centred in a 20^3 grid of 0.25 m voxels.
        {
            const uint32_t n = 20;
            const float radius = 7.0f;
            const VoxelGridDesc desc = MakeDesc(n, n, n, 0.25f);
            OccupancyBitGrid grid(n, n, n);
            for (uint32_t z = 0; z < n; ++z)
                for (uint32_t y = 0; y < n; ++y)
                    for (uint32_t x = 0; x < n; ++x)
                    {
                        const float dx = float(x) + 0.5f - 0.5f * float(n);
                        const float dy = float(y) + 0.5f - 0.5f * float(n);
                        const float dz = float(z) + 0.5f - 0.5f * float(n);
                        if (dx * dx + dy * dy + dz * dz <= radius * radius)
                            grid.Set(x, y, z);
                    }
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            Fluid::IsoSurfaceMesh mesh;
            ExtractSdfIsoSurface(field, desc, mesh);

            const float vs3 = desc.voxelSize * desc.voxelSize * desc.voxelSize;
            const float volume = Fluid::IsoSurfaceVolume(mesh);
            const float voxelVolume = float(grid.CountOccupied()) * vs3;
            const float sphereVolume = 4.0f / 3.0f * PI * radius * radius * radius * vs3;
            check(mesh.TriangleCount() > 0, "sphere: mesh has triangles");
            check(Fluid::CountOpenEdges(mesh) == 0, "sphere: mesh is closed");
            check(volume > 0.0f, "sphere: normals point outward (positive volume)");
            // The surface runs through the voxel faces and cuts their corners, so it holds a little less
            // than the voxels; it should stay within a few percent of both them and the analytic sphere.
            check(std::fabs(volume - voxelVolume) <= 0.08f * voxelVolume, "sphere: volume within 8% of the voxels");
            check(std::fabs(volume - sphereVolume) <= 0.08f * sphereVolume, "sphere: volume within 8% of 4/3 pi r^3");
            check(VerticesInsideGrownGrid(mesh, desc, 0.0f), "sphere: vertices inside the grid");
            info << " sphereTris=" << mesh.TriangleCount() << " sphereVol/voxels=" << volume / voxelVolume
                 << " sphereVol/analytic=" << volume / sphereVolume;
        }

        // (b) Box touching the -x, -y and +z borders: the extractor caps it outside the grid.
        {
            const VoxelGridDesc desc = MakeDesc(9, 7, 8, 0.5f);
            OccupancyBitGrid grid(9, 7, 8);
            for (uint32_t z = 3; z < 8; ++z)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 5; ++x)
                        grid.Set(x, y, z);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            Fluid::IsoSurfaceMesh mesh;
            ExtractSdfIsoSurface(field, desc, mesh);
            const float volume = Fluid::IsoSurfaceVolume(mesh);
            const float voxelVolume = float(grid.CountOccupied()) * 0.125f;
            check(mesh.TriangleCount() > 0 && Fluid::CountOpenEdges(mesh) == 0, "border box: closed mesh");
            check(volume > 0.0f, "border box: positive volume");
            check(VerticesInsideGrownGrid(mesh, desc, 0.5f), "border box: caps within half a voxel of the grid");
            // 5x4x5 voxels; the caps on the -x (4x5), -y (5x5) and +z (5x4) faces add at most half a voxel each.
            const float capBound = 0.5f * (20.0f + 25.0f + 20.0f) * 0.125f;
            check(volume <= voxelVolume + capBound + 1e-3f, "border box: volume at most voxels + half-voxel caps");
            info << " boxVol/voxels=" << volume / voxelVolume;
        }

        // (c) One voxel: a small closed blob inside its own cell, no bigger than the voxel.
        {
            const VoxelGridDesc desc = MakeDesc(5, 5, 5, 1.0f);
            OccupancyBitGrid grid(5, 5, 5);
            grid.Set(2, 2, 2);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            Fluid::IsoSurfaceMesh mesh;
            ExtractSdfIsoSurface(field, desc, mesh);
            const float volume = Fluid::IsoSurfaceVolume(mesh);
            check(mesh.TriangleCount() > 0 && Fluid::CountOpenEdges(mesh) == 0, "single voxel: closed mesh");
            check(volume > 0.0f && volume <= 1.0f + 1e-4f, "single voxel: volume in (0, 1]");
        }

        // (d) Empty occupancy and a desc that does not match the field.
        {
            const VoxelGridDesc desc = MakeDesc(4, 4, 4, 1.0f);
            SignedDistanceGrid field;
            BuildSignedDistanceField(OccupancyBitGrid(4, 4, 4), field);
            Fluid::IsoSurfaceMesh mesh;
            ExtractSdfIsoSurface(field, desc, mesh);
            check(mesh.TriangleCount() == 0, "empty occupancy: no triangles");

            OccupancyBitGrid one(4, 4, 4);
            one.Set(1, 1, 1);
            BuildSignedDistanceField(one, field);
            ExtractSdfIsoSurface(field, MakeDesc(4, 4, 5, 1.0f), mesh);
            check(mesh.TriangleCount() == 0, "mismatched desc: no triangles");

            ExtractSdfIsoSurface(SignedDistanceGrid(), MakeDesc(0, 0, 0, 1.0f), mesh);
            check(mesh.TriangleCount() == 0, "empty field: no triangles");
        }

        std::ostringstream out;
        if (!firstError.empty())
        {
            out << "ERR " << firstError << " (checks=" << checks << ")";
        }
        else
        {
            out << "OK checks=" << checks << info.str();
        }
        return out.str();
    }
}
