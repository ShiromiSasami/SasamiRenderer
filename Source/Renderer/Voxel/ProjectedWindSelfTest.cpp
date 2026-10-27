#include "Renderer/Voxel/ProjectedWindSelfTest.h"

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/GustedWindField.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/SdfWindField.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"
#include "Renderer/Voxel/WindParticleSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr uint32_t GRID_DIM = 32;
        constexpr float VOXEL_SIZE = 0.25f;
        constexpr float WIND_X = 3.0f;

        VoxelGridDesc MakeDesc()
        {
            VoxelGridDesc desc;
            desc.origin[0] = 0.0f;
            desc.origin[1] = 0.0f;
            desc.origin[2] = 0.0f;
            desc.voxelSize = VOXEL_SIZE;
            desc.dimX = desc.dimY = desc.dimZ = GRID_DIM;
            return desc;
        }

        ProjectedWindSettings MakeSettings()
        {
            ProjectedWindSettings settings;
            settings.wind[0] = WIND_X;
            settings.wind[1] = 0.0f;
            settings.wind[2] = 0.0f;
            return settings;
        }

        // Fills [x0, x1) x [y0, y1) x [z0, z1).
        void FillBox(OccupancyBitGrid& grid, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, uint32_t z0, uint32_t z1)
        {
            for (uint32_t z = z0; z < z1; ++z)
                for (uint32_t y = y0; y < y1; ++y)
                    for (uint32_t x = x0; x < x1; ++x)
                        grid.Set(x, y, z);
        }

        float Length3(const float v[3])
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }
    }

    std::string RunProjectedWindSelfTest()
    {
        uint32_t checks = 0;
        std::string firstError;
        auto check = [&](bool condition, const std::string& description)
        {
            ++checks;
            if (!condition && firstError.empty()) {
                firstError = description;
            }
        };

        const VoxelGridDesc desc = MakeDesc();
        const ProjectedWindSettings settings = MakeSettings();

        // (a) No obstacle: the uniform wind survives the projection.
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            const ProjectedWindField wind(grid, field, desc, settings);
            const PressureProjectionStats& stats = wind.Stats();
            check(stats.valid, "empty: stats.valid");
            check(stats.enclosedCells == 0, "empty: enclosedCells == 0");
            const StaggeredVelocityGrid& v = wind.Grid();
            float worst = 0.0f;
            for (uint32_t k = 0; k < GRID_DIM; ++k)
                for (uint32_t j = 0; j < GRID_DIM; ++j)
                    for (uint32_t i = 0; i <= GRID_DIM; ++i)
                        worst = std::max(worst, std::fabs(v.U(i, j, k) - WIND_X));
            for (uint32_t k = 0; k < GRID_DIM; ++k)
                for (uint32_t j = 0; j <= GRID_DIM; ++j)
                    for (uint32_t i = 0; i < GRID_DIM; ++i)
                        worst = std::max(worst, std::fabs(v.V(i, j, k)));
            for (uint32_t k = 0; k <= GRID_DIM; ++k)
                for (uint32_t j = 0; j < GRID_DIM; ++j)
                    for (uint32_t i = 0; i < GRID_DIM; ++i)
                        worst = std::max(worst, std::fabs(v.W(i, j, k)));
            std::ostringstream detail;
            detail << "empty: worst |face - wind| = " << worst;
            check(worst < 1.0e-4f, detail.str());
        }

        // (b) A building standing on the floor.
        uint64_t penetrations = 0;
        double rate = 0.0;
        uint32_t iterations = 0;
        float finalDivergence = 0.0f;
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            FillBox(grid, 12, 20, 0, 10, 12, 20);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            const ProjectedWindField wind(grid, field, desc, settings);
            const PressureProjectionStats& stats = wind.Stats();
            iterations = stats.iterations;
            finalDivergence = stats.finalMaxDivergence;
            check(stats.valid, "box: stats.valid");
            std::ostringstream divDetail;
            divDetail << "box: finalMaxDivergence = " << stats.finalMaxDivergence;
            check(stats.finalMaxDivergence <= 1.0e-2f, divDetail.str());
            check(stats.converged, "box: converged");

            const StaggeredVelocityGrid& v = wind.Grid();
            float worstSolidFlux = 0.0f;
            for (uint32_t k = 12; k < 20; ++k)
                for (uint32_t j = 0; j < 10; ++j)
                    for (uint32_t i = 12; i <= 20; ++i)
                        worstSolidFlux = std::max(worstSolidFlux, std::fabs(v.U(i, j, k)));
            for (uint32_t k = 12; k < 20; ++k)
                for (uint32_t j = 0; j <= 10; ++j)
                    for (uint32_t i = 12; i < 20; ++i)
                        worstSolidFlux = std::max(worstSolidFlux, std::fabs(v.V(i, j, k)));
            for (uint32_t k = 12; k <= 20; ++k)
                for (uint32_t j = 0; j < 10; ++j)
                    for (uint32_t i = 12; i < 20; ++i)
                        worstSolidFlux = std::max(worstSolidFlux, std::fabs(v.W(i, j, k)));
            std::ostringstream fluxDetail;
            fluxDetail << "box: worst face flux touching the box = " << worstSolidFlux;
            check(worstSolidFlux == 0.0f, fluxDetail.str());

            float worstFloor = 0.0f;
            for (uint32_t k = 0; k < GRID_DIM; ++k)
                for (uint32_t i = 0; i < GRID_DIM; ++i)
                    worstFloor = std::max(worstFloor, std::fabs(v.V(i, 0, k)));
            check(worstFloor == 0.0f, "box: y = 0 faces are zero");

            // Voxel point (11.0, 5.5, 16.0) is the middle of the upwind face's neighbour column.
            const float front[3] = { 11.0f * VOXEL_SIZE, 5.5f * VOXEL_SIZE, 16.0f * VOXEL_SIZE };
            float frontV[3];
            wind.Velocity(front, frontV);
            std::ostringstream frontDetail;
            frontDetail << "box: front v.x = " << frontV[0];
            check(frontV[0] < 0.5f * WIND_X, frontDetail.str());

            const float over[3] = { 16.0f * VOXEL_SIZE, 12.0f * VOXEL_SIZE, 16.0f * VOXEL_SIZE };
            float overV[3];
            wind.Velocity(over, overV);
            std::ostringstream overDetail;
            overDetail << "box: |v| over the roof = " << Length3(overV);
            check(Length3(overV) > WIND_X, overDetail.str());

            const uint32_t count = 2000;
            const uint32_t steps = 120;
            const float dt = 0.5f * VOXEL_SIZE / WIND_X;
            WindParticleSystem particles;
            particles.Reset(wind, count, 1u, 40.0f * dt, 120.0f * dt, VOXEL_SIZE);
            for (uint32_t i = 0; i < steps; ++i) {
                penetrations += particles.Step(wind, dt).respawnedSolid;
            }
            rate = static_cast<double>(penetrations) / (static_cast<double>(count) * steps);
            std::ostringstream rateDetail;
            rateDetail << "box: particle penetration rate " << rate;
            check(rate < 0.01, rateDetail.str());
        }

        // (d) The same building with curl-noise gusts on top (6a-4a): the flow changes, the gust part stays
        // nearly divergence-free, and particles still rarely enter the block.
        double gustRate = 0.0;
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            FillBox(grid, 12, 20, 0, 10, 12, 20);
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            const ProjectedWindField base(grid, field, desc, settings);
            SdfWindSettings gustSettings;
            gustSettings.wind[0] = WIND_X;
            gustSettings.wind[1] = 0.0f;
            gustSettings.wind[2] = 0.0f;
            gustSettings.rampWidth = 4.0f * VOXEL_SIZE;
            gustSettings.noiseScale = 8.0f * VOXEL_SIZE;
            gustSettings.turbulence = 0.3f;
            gustSettings.laminar = false;
            SdfWindField gusts(field, desc, gustSettings);
            const GustedWindField wind(base, gusts);

            // Sample a lattice in open air, one voxel steps, away from the grid edge.
            double gustSum = 0.0;
            double gustSpeedSum = 0.0;
            uint32_t samples = 0;
            const float h = 0.5f * VOXEL_SIZE;
            for (uint32_t k = 2; k < GRID_DIM - 2; k += 3)
                for (uint32_t j = 2; j < GRID_DIM - 2; j += 3)
                    for (uint32_t i = 2; i < GRID_DIM - 2; i += 3) {
                        const float p[3] = { (i + 0.5f) * VOXEL_SIZE, (j + 0.5f) * VOXEL_SIZE, (k + 0.5f) * VOXEL_SIZE };
                        if (wind.Distance(p) < 2.0f * VOXEL_SIZE) {
                            continue;
                        }
                        float total[3];
                        float baseV[3];
                        wind.Velocity(p, total);
                        base.Velocity(p, baseV);
                        const float delta[3] = { total[0] - baseV[0], total[1] - baseV[1], total[2] - baseV[2] };
                        gustSpeedSum += Length3(delta);
                        float divergence = 0.0f;
                        for (int axis = 0; axis < 3; ++axis) {
                            float plus[3] = { p[0], p[1], p[2] };
                            float minus[3] = { p[0], p[1], p[2] };
                            plus[axis] += h;
                            minus[axis] -= h;
                            float vPlus[3];
                            float vMinus[3];
                            gusts.Velocity(plus, vPlus);
                            gusts.Velocity(minus, vMinus);
                            divergence += (vPlus[axis] - vMinus[axis]) / (2.0f * h);
                        }
                        gustSum += std::fabs(divergence);
                        ++samples;
                    }
            const double meanGust = samples ? gustSpeedSum / samples : 0.0;
            // |div| in 1/s, relative to |gust| / voxel, the size one gradient of the gust would have.
            const double meanDiv = samples ? gustSum / samples : 0.0;
            std::ostringstream gustDetail;
            gustDetail << "gusts: mean |v - base| = " << meanGust << " over " << samples << " samples";
            check(samples > 100 && meanGust > 0.05 * WIND_X, gustDetail.str());
            std::ostringstream divDetail;
            divDetail << "gusts: mean |div v_gust| = " << meanDiv << " vs |v_gust| / voxel = " << meanGust / VOXEL_SIZE;
            check(meanDiv < 0.05 * meanGust / VOXEL_SIZE, divDetail.str());

            const uint32_t count = 2000;
            const uint32_t steps = 120;
            const float dt = 0.5f * VOXEL_SIZE / WIND_X;
            WindParticleSystem particles;
            particles.Reset(wind, count, 1u, 40.0f * dt, 120.0f * dt, VOXEL_SIZE);
            uint64_t gustPenetrations = 0;
            for (uint32_t i = 0; i < steps; ++i) {
                gusts.SetTime(static_cast<float>(i) * dt);
                gustPenetrations += particles.Step(wind, dt).respawnedSolid;
            }
            gustRate = static_cast<double>(gustPenetrations) / (static_cast<double>(count) * steps);
            std::ostringstream rateDetail;
            rateDetail << "gusts: particle penetration rate " << gustRate;
            check(gustRate < 0.01, rateDetail.str());
        }

        // (c) A sealed hollow shell: the inside is enclosed and still.
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            for (uint32_t z = 8; z < 24; ++z)
                for (uint32_t y = 8; y < 24; ++y)
                    for (uint32_t x = 8; x < 24; ++x) {
                        const bool surface = x == 8 || x == 23 || y == 8 || y == 23 || z == 8 || z == 23;
                        if (surface) {
                            grid.Set(x, y, z);
                        }
                    }
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            const ProjectedWindField wind(grid, field, desc, settings);
            const PressureProjectionStats& stats = wind.Stats();
            check(stats.valid, "shell: stats.valid");
            std::ostringstream enclosedDetail;
            enclosedDetail << "shell: enclosedCells = " << stats.enclosedCells;
            check(stats.enclosedCells == 14u * 14u * 14u, enclosedDetail.str());
            const float inside[3] = { 16.0f * VOXEL_SIZE, 16.0f * VOXEL_SIZE, 16.0f * VOXEL_SIZE };
            float insideV[3];
            wind.Velocity(inside, insideV);
            std::ostringstream insideDetail;
            insideDetail << "shell: |v| inside = " << Length3(insideV);
            check(Length3(insideV) < 1.0e-6f, insideDetail.str());
        }

        std::ostringstream out;
        if (firstError.empty()) {
            out << "OK checks=" << checks << " penetrations=" << (100.0 * rate) << "% gustPenetrations=" << (100.0 * gustRate) << "% iters=" << iterations
                << " finalDiv=" << finalDivergence;
        } else {
            out << "ERR " << firstError << " (checks=" << checks << ")";
        }
        return out.str();
    }
}
