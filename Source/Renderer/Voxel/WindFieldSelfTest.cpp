#include "Renderer/Voxel/WindFieldSelfTest.h"

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
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

        // Deterministic LCG, uniform in [0, 1).
        float NextUnit(uint32_t& state)
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8u) * (1.0f / 16777216.0f);
        }

        float Length3(const float v[3])
        {
            return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }

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

        // One occupied voxel in the (0,0,0) corner.
        OccupancyBitGrid MakeCornerGrid()
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            grid.Set(0, 0, 0);
            return grid;
        }

        // Voxels with y < 8 occupied: the plane y = 2.0 m.
        OccupancyBitGrid MakeFloorGrid()
        {
            OccupancyBitGrid grid(GRID_DIM, GRID_DIM, GRID_DIM);
            for (uint32_t z = 0; z < GRID_DIM; ++z)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < GRID_DIM; ++x)
                        grid.Set(x, y, z);
            return grid;
        }

        void Sample(const SdfWindField& wind, float x, float y, float z, float out[3])
        {
            const float p[3] = { x, y, z };
            wind.Velocity(p, out);
        }
    }

    std::string RunWindFieldSelfTest()
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

        // (a) The ramp of eq. (4).
        {
            check(CurlNoiseRamp(0.0f) == 0.0f, "ramp: ramp(0) == 0");
            check(CurlNoiseRamp(1.0f) == 1.0f, "ramp: ramp(1) == 1");
            check(CurlNoiseRamp(-1.0f) == -1.0f, "ramp: ramp(-1) == -1");
            check(CurlNoiseRamp(2.0f) == 1.0f, "ramp: ramp(2) clamps to 1");
            const float expectedHalf = 15.0f / 16.0f - 10.0f / 64.0f + 3.0f / 256.0f;
            check(std::fabs(CurlNoiseRamp(0.5f) - expectedHalf) < 1.0e-6f, "ramp: ramp(0.5) matches the polynomial");
            bool monotonic = true;
            float previous = CurlNoiseRamp(-1.0f);
            for (uint32_t i = 1; i < 100; ++i) {
                const float r = -1.0f + 2.0f * static_cast<float>(i) / 99.0f;
                const float value = CurlNoiseRamp(r);
                if (value < previous) {
                    monotonic = false;
                }
                previous = value;
            }
            check(monotonic, "ramp: monotonic on [-1, 1]");
        }

        // (b) Far from every solid and without turbulence the velocity is the wind.
        {
            OccupancyBitGrid grid = MakeCornerGrid();
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            SdfWindSettings settings;
            settings.wind[0] = 2.0f;
            settings.wind[1] = 0.5f;
            settings.wind[2] = -1.0f;
            settings.rampWidth = 0.5f;
            settings.turbulence = 0.0f;
            const SdfWindField wind(field, desc, settings);
            const float windLength = Length3(settings.wind);

            uint32_t state = 12345u;
            float worst = 0.0f;
            for (uint32_t i = 0; i < 20; ++i) {
                const float p[3] = { 3.0f + 2.0f * NextUnit(state), 3.0f + 2.0f * NextUnit(state), 3.0f + 2.0f * NextUnit(state) };
                float v[3];
                wind.Velocity(p, v);
                const float diff[3] = { v[0] - settings.wind[0], v[1] - settings.wind[1], v[2] - settings.wind[2] };
                worst = std::max(worst, Length3(diff));
            }
            std::ostringstream detail;
            detail << "free field: worst |v - wind| = " << worst;
            check(worst < 2.0e-3f * windLength, detail.str());
        }

        // (c) Divergence with turbulence on.
        {
            OccupancyBitGrid grid = MakeCornerGrid();
            SignedDistanceGrid field;
            BuildSignedDistanceField(grid, field);
            SdfWindSettings settings;
            settings.wind[0] = 2.0f;
            settings.wind[1] = 0.0f;
            settings.wind[2] = 0.0f;
            settings.rampWidth = 0.5f;
            settings.turbulence = 0.5f;
            settings.noiseScale = 2.0f;
            const SdfWindField wind(field, desc, settings);
            const float windLength = Length3(settings.wind);

            const float e = 0.125f;
            uint32_t state = 777u;
            float worstDivergence = 0.0f;
            float minSpeed = 1.0e30f;
            float maxSpeed = 0.0f;
            for (uint32_t i = 0; i < 20; ++i) {
                const float x = 3.0f + 2.0f * NextUnit(state);
                const float y = 3.0f + 2.0f * NextUnit(state);
                const float z = 3.0f + 2.0f * NextUnit(state);
                float xp[3], xm[3], yp[3], ym[3], zp[3], zm[3], center[3];
                Sample(wind, x + e, y, z, xp);
                Sample(wind, x - e, y, z, xm);
                Sample(wind, x, y + e, z, yp);
                Sample(wind, x, y - e, z, ym);
                Sample(wind, x, y, z + e, zp);
                Sample(wind, x, y, z - e, zm);
                Sample(wind, x, y, z, center);
                const float divergence = ((xp[0] - xm[0]) + (yp[1] - ym[1]) + (zp[2] - zm[2])) / (2.0f * e);
                worstDivergence = std::max(worstDivergence, std::fabs(divergence));
                const float speed = Length3(center);
                minSpeed = std::min(minSpeed, speed);
                maxSpeed = std::max(maxSpeed, speed);
            }
            std::ostringstream detail;
            detail << "divergence: worst |div| * e / |wind| = " << (worstDivergence * e / windLength);
            check(worstDivergence * e / windLength < 0.05f, detail.str());
            check(maxSpeed - minSpeed > 0.05f * windLength, "divergence: turbulence changes |v|");
        }

        // (d) Wall: the normal component dies at the floor, the far field is untouched.
        OccupancyBitGrid floorGrid = MakeFloorGrid();
        SignedDistanceGrid floorField;
        BuildSignedDistanceField(floorGrid, floorField);
        SdfWindSettings floorSettings;
        floorSettings.wind[0] = 2.0f;
        floorSettings.wind[1] = -1.0f;
        floorSettings.wind[2] = 0.0f;
        floorSettings.rampWidth = 1.0f;
        floorSettings.turbulence = 0.0f;
        {
            const SdfWindField wind(floorField, desc, floorSettings);
            uint32_t state = 4242u;
            float worstNormal = 0.0f;
            float minSlide = 1.0e30f;
            float worstFarY = 0.0f;
            float worstFarX = 0.0f;
            for (uint32_t i = 0; i < 20; ++i) {
                const float x = 3.0f + 2.0f * NextUnit(state);
                const float z = 3.0f + 2.0f * NextUnit(state);
                float near[3];
                Sample(wind, x, 2.0f + 0.1f, z, near);
                worstNormal = std::max(worstNormal, std::fabs(near[1]));
                minSlide = std::min(minSlide, near[0]);
                float far[3];
                Sample(wind, x, 2.0f + 3.0f, z, far);
                worstFarY = std::max(worstFarY, std::fabs(far[1] - (-1.0f)));
                worstFarX = std::max(worstFarX, std::fabs(far[0] - 2.0f));
            }
            std::ostringstream normalDetail;
            normalDetail << "wall: worst |v.y| near the floor = " << worstNormal;
            check(worstNormal < 0.3f, normalDetail.str());
            std::ostringstream slideDetail;
            slideDetail << "wall: min v.x near the floor = " << minSlide;
            check(minSlide > 0.0f, slideDetail.str());
            std::ostringstream farYDetail;
            farYDetail << "wall: worst far |v.y + 1| = " << worstFarY;
            check(worstFarY < 0.02f, farYDetail.str());
            std::ostringstream farXDetail;
            farXDetail << "wall: worst far |v.x - 2| = " << worstFarX;
            check(worstFarX < 0.02f, farXDetail.str());
        }

        // (e) Particles blown into the floor.
        uint64_t penetrations = 0;
        double lastMeanSpeed = 0.0;
        {
            const SdfWindField wind(floorField, desc, floorSettings);
            const uint32_t count = 2000;
            const uint32_t steps = 200;
            WindParticleSystem particles;
            particles.Reset(wind, count, 7u, 2.0f, 4.0f, 0.25f);
            float maxSpeed = 0.0f;
            for (uint32_t i = 0; i < steps; ++i) {
                const WindParticleSystem::StepStats stats = particles.Step(wind, 0.02f);
                penetrations += stats.respawnedSolid;
                lastMeanSpeed = stats.meanSpeed;
                maxSpeed = stats.maxSpeed;
            }
            const double rate = static_cast<double>(penetrations) / (static_cast<double>(count) * steps);
            std::ostringstream rateDetail;
            rateDetail << "particles: penetration rate " << rate;
            check(rate < 0.002, rateDetail.str());
            std::ostringstream meanDetail;
            meanDetail << "particles: last meanSpeed " << lastMeanSpeed;
            check(lastMeanSpeed > 0.5, meanDetail.str());
            const float windLength = Length3(floorSettings.wind);
            std::ostringstream maxDetail;
            maxDetail << "particles: last maxSpeed " << maxSpeed;
            check(maxSpeed <= 4.0f * windLength + 1.0e-3f, maxDetail.str());
        }

        std::ostringstream out;
        if (firstError.empty()) {
            out << "OK checks=" << checks << " penetrations=" << penetrations << " meanSpeed=" << lastMeanSpeed;
        } else {
            out << "ERR " << firstError << " (checks=" << checks << ")";
        }
        return out.str();
    }
}
