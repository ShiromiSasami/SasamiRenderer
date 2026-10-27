#include "Renderer/Voxel/CpuSolidVoxelizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        // Edge function in the YZ plane with endpoints in canonical (lexicographic) order, so the
        // same double expression results no matter which triangle evaluates a shared edge.
        double CanonicalEdgeFunction(const double* a, const double* b, double py, double pz)
        {
            const bool aFirst = a[1] < b[1] || (a[1] == b[1] && a[2] < b[2]);
            const double* p = aFirst ? a : b;
            const double* q = aFirst ? b : a;
            return (q[1] - p[1]) * (pz - p[2]) - (q[2] - p[2]) * (py - p[1]);
        }

        // Signed distance-like test of voxel centre x against the triangle's plane along +X,
        // scaled by |nx| so that no division is needed: positive when the centre lies at larger x
        // than the crossing (cx > v0x - b / nx). b = ny * (yc - v0y) + nz * (zc - v0z).
        // Every operation is a correctly rounded add or multiply, so the value is monotone
        // non-decreasing in x and reproducible bit for bit by the GPU port.
        bool CentrePastCrossing(double nx, double b, double v0x, double originX, double voxelSize, int64_t x)
        {
            const double cx = originX + (static_cast<double>(x) + 0.5) * voxelSize;
            const double absNx = nx > 0.0 ? nx : -nx;
            const double signedB = nx > 0.0 ? b : -b;
            return absNx * (cx - v0x) + signedB > 0.0;
        }

        // Smallest x in [0, dimX) whose centre lies past the crossing, or dimX when none does.
        // A binary search over the monotone predicate, so the answer is the same however it is
        // searched for.
        int64_t FirstVoxelPastCrossing(double nx, double b, double v0x, double originX, double voxelSize, uint32_t dimX)
        {
            int64_t lo = 0;
            int64_t hi = dimX;
            while (lo < hi)
            {
                const int64_t mid = (lo + hi) / 2;
                if (CentrePastCrossing(nx, b, v0x, originX, voxelSize, mid))
                {
                    hi = mid;
                }
                else
                {
                    lo = mid + 1;
                }
            }
            return lo;
        }
    }

    SolidVoxelizeStats VoxelizeSolidParity(const VoxelGridDesc& desc,
                                           const VoxelizerMeshInput& mesh,
                                           OccupancyBitGrid& grid)
    {
        SolidVoxelizeStats stats;

        const bool invalid = desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0
            || !(desc.voxelSize > 0.0f)
            || mesh.positions == nullptr
            || grid.DimX() != desc.dimX || grid.DimY() != desc.dimY || grid.DimZ() != desc.dimZ;
        if (invalid)
        {
            stats.occupiedVoxels = grid.CountOccupied();
            return stats;
        }

        const uint32_t triangleCount = (mesh.indices != nullptr ? mesh.indexCount : mesh.vertexCount) / 3;
        const double voxelSize = desc.voxelSize;
        const double originX = desc.origin[0];
        const double originY = desc.origin[1];
        const double originZ = desc.origin[2];
        const int64_t maxJ = static_cast<int64_t>(desc.dimY) - 1;
        const int64_t maxK = static_cast<int64_t>(desc.dimZ) - 1;

        for (uint32_t tri = 0; tri < triangleCount; ++tri)
        {
            ++stats.trianglesTested;

            uint32_t vertexIndex[3];
            bool outOfRange = false;
            for (uint32_t k = 0; k < 3; ++k)
            {
                vertexIndex[k] = mesh.indices != nullptr ? mesh.indices[tri * 3 + k] : tri * 3 + k;
                if (vertexIndex[k] >= mesh.vertexCount)
                {
                    outOfRange = true;
                }
            }
            if (outOfRange)
            {
                ++stats.trianglesSkipped;
                continue;
            }

            double v[3][3];
            for (uint32_t k = 0; k < 3; ++k)
            {
                const float* p = mesh.positions + static_cast<size_t>(vertexIndex[k]) * mesh.positionStrideInFloats;
                v[k][0] = p[0];
                v[k][1] = p[1];
                v[k][2] = p[2];
            }

            const double d = (v[1][1] - v[0][1]) * (v[2][2] - v[0][2]) - (v[1][2] - v[0][2]) * (v[2][1] - v[0][1]);
            if (d == 0.0)
            {
                ++stats.trianglesSkipped;
                continue;
            }

            const double minY = (std::min)(v[0][1], (std::min)(v[1][1], v[2][1]));
            const double maxY = (std::max)(v[0][1], (std::max)(v[1][1], v[2][1]));
            const double minZ = (std::min)(v[0][2], (std::min)(v[1][2], v[2][2]));
            const double maxZ = (std::max)(v[0][2], (std::max)(v[1][2], v[2][2]));
            // The row range only has to be a superset of the covered centres: the exact coverage
            // test below rejects the extra rows. Widening by one voxel makes the range independent
            // of how it was rounded, so the GPU port may compute it in float.
            const int64_t jMin = (std::max)(static_cast<int64_t>(0), static_cast<int64_t>(std::floor((minY - originY) / voxelSize - 0.5)) - 1);
            const int64_t jMax = (std::min)(maxJ, static_cast<int64_t>(std::floor((maxY - originY) / voxelSize - 0.5)) + 1);
            const int64_t kMin = (std::max)(static_cast<int64_t>(0), static_cast<int64_t>(std::floor((minZ - originZ) / voxelSize - 0.5)) - 1);
            const int64_t kMax = (std::min)(maxK, static_cast<int64_t>(std::floor((maxZ - originZ) / voxelSize - 0.5)) + 1);
            if (jMin > jMax || kMin > kMax)
            {
                ++stats.trianglesSkipped;
                continue;
            }

            // Edge i is (v[i], v[(i+1)%3]) with opposite vertex v[(i+2)%3].
            double edgeSign[3];
            for (uint32_t i = 0; i < 3; ++i)
            {
                const double* c = v[(i + 2) % 3];
                const double e = CanonicalEdgeFunction(v[i], v[(i + 1) % 3], c[1], c[2]);
                edgeSign[i] = e > 0.0 ? 1.0 : -1.0;
            }

            const double nx = d;
            const double ny = (v[1][2] - v[0][2]) * (v[2][0] - v[0][0]) - (v[1][0] - v[0][0]) * (v[2][2] - v[0][2]);
            const double nz = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[1][1] - v[0][1]) * (v[2][0] - v[0][0]);

            // Schwarz & Seidel 2010, sec. 4: flip every voxel behind each crossing along +X.
            for (int64_t k = kMin; k <= kMax; ++k)
            {
                const double zc = originZ + (static_cast<double>(k) + 0.5) * voxelSize;
                for (int64_t j = jMin; j <= jMax; ++j)
                {
                    const double yc = originY + (static_cast<double>(j) + 0.5) * voxelSize;

                    bool covered = true;
                    for (uint32_t i = 0; i < 3 && covered; ++i)
                    {
                        const double e = CanonicalEdgeFunction(v[i], v[(i + 1) % 3], yc, zc);
                        covered = e * edgeSign[i] > 0.0 || (e == 0.0 && edgeSign[i] > 0.0);
                    }
                    if (!covered)
                    {
                        continue;
                    }

                    const double b = ny * (yc - v[0][1]) + nz * (zc - v[0][2]);

                    const int64_t q = FirstVoxelPastCrossing(nx, b, v[0][0], originX, voxelSize, desc.dimX);
                    if (q >= static_cast<int64_t>(desc.dimX))
                    {
                        continue;
                    }

                    for (int64_t x = q; x < static_cast<int64_t>(desc.dimX); ++x)
                    {
                        grid.Flip(static_cast<uint32_t>(x), static_cast<uint32_t>(j), static_cast<uint32_t>(k));
                    }
                    ++stats.rowFlips;
                }
            }
        }

        stats.occupiedVoxels = grid.CountOccupied();
        return stats;
    }
}
