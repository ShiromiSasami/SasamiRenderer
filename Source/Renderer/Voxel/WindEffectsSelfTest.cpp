#include "Renderer/Voxel/WindEffectsSelfTest.h"

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/FlameColor.h"
#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/WindDrivenParticles.h"
#include "Renderer/Voxel/WindFireSpread.h"
#include "Renderer/Voxel/WindVelocityField.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float STEP_DT = 1.0f / 60.0f;
        constexpr float NO_FLOOR = -1e30f;

        // Uniform wind in a box, optionally with a solid half-space y < floorY.
        class TestField final : public IWindVelocityField
        {
        public:
            TestField(const VoxelGridDesc& desc, float windX, float floorY)
                : m_desc(desc), m_floorY(floorY)
            {
                m_wind[0] = windX;
            }

            const VoxelGridDesc& Desc() const override { return m_desc; }

            float Distance(const float position[3]) const override
            {
                return m_floorY <= NO_FLOOR ? 1e9f : position[1] - m_floorY;
            }

            void Velocity(const float position[3], float outVelocity[3]) const override
            {
                (void)position;
                for (int a = 0; a < 3; ++a)
                    outVelocity[a] = m_wind[a];
            }

            bool Contains(const float position[3]) const override
            {
                const float extent[3] = {
                    static_cast<float>(m_desc.dimX) * m_desc.voxelSize,
                    static_cast<float>(m_desc.dimY) * m_desc.voxelSize,
                    static_cast<float>(m_desc.dimZ) * m_desc.voxelSize,
                };
                for (int a = 0; a < 3; ++a)
                    if (position[a] < m_desc.origin[a] || position[a] > m_desc.origin[a] + extent[a])
                        return false;
                return true;
            }

        private:
            VoxelGridDesc m_desc;
            float m_wind[3] = { 0.0f, 0.0f, 0.0f };
            float m_floorY;
        };

        VoxelGridDesc MakeDesc(uint32_t dimX, uint32_t dimY, uint32_t dimZ, float ox, float oy, float oz)
        {
            VoxelGridDesc desc;
            desc.origin[0] = ox;
            desc.origin[1] = oy;
            desc.origin[2] = oz;
            desc.voxelSize = 1.0f;
            desc.dimX = dimX;
            desc.dimY = dimY;
            desc.dimZ = dimZ;
            return desc;
        }

        WindDrivenParticleSettings MakeBaseSettings(WindParticleKind kind)
        {
            WindDrivenParticleSettings s;
            s.kind = kind;
            s.maxParticles = 1;
            s.spawnRate = 0.0f;
            s.minLifetime = 100.0f;
            s.maxLifetime = 100.0f;
            s.dragTime = 0.2f;
            s.velocityJitter = 0.0f;
            s.flutterAcceleration = 0.0f;
            return s;
        }

        void RunParticle(WindDrivenParticles& particles, const IWindVelocityField& field, const WindDrivenParticleSettings& settings,
            float startY, float seconds)
        {
            particles.Reset(settings, 1u);
            const float start[3] = { 0.0f, startY, 0.0f };
            particles.Emit(start, 0.0f, 1);
            const int steps = static_cast<int>(seconds / STEP_DT + 0.5f);
            for (int i = 0; i < steps; ++i)
                particles.Step(field, STEP_DT);
        }

        struct FireExtent
        {
            int down = 0;
            int up = 0;
            int side = 0;
            float radius = 0.0f;   // farthest burning cell from the centre, in voxels (xz)
        };

        FireExtent RunFire(float windX, uint32_t seed, float maxSpreadRadius = 0.0f)
        {
            constexpr uint32_t DIM = 41;
            constexpr int CENTRE = 20;
            const VoxelGridDesc desc = MakeDesc(DIM, 3, DIM, 0.0f, 0.0f, 0.0f);
            OccupancyBitGrid grid(DIM, 3, DIM);
            for (uint32_t z = 0; z < DIM; ++z)
                for (uint32_t x = 0; x < DIM; ++x)
                    grid.Set(x, 0, z);

            const TestField field(desc, windX, NO_FLOOR);
            WindFireSettings settings;
            settings.burnTime = 1000.0f;
            settings.maxSpreadRadius = maxSpreadRadius;
            WindFireSpread fire;
            fire.Build(grid, desc, settings, seed);
            const float ignitePoint[3] = { 20.5f, 0.5f, 20.5f };
            fire.Ignite(ignitePoint);
            // 10 ticks: the front moves at most one cell per tick, so it stays inside the 20-cell half-width.
            for (int i = 0; i < 10; ++i)
                fire.Step(field, 0.25f);

            FireExtent extent;
            for (const uint32_t linear : fire.BurningCells())
            {
                const int x = static_cast<int>(linear % DIM);
                const int z = static_cast<int>(linear / (DIM * 3));
                extent.down = (std::max)(extent.down, x - CENTRE);
                extent.up = (std::max)(extent.up, CENTRE - x);
                extent.side = (std::max)(extent.side, std::abs(z - CENTRE));
                const float r2 = static_cast<float>((x - CENTRE) * (x - CENTRE) + (z - CENTRE) * (z - CENTRE));
                extent.radius = (std::max)(extent.radius, std::sqrt(r2));
            }
            return extent;
        }

        std::string Err(const char* name, float a, float b)
        {
            std::ostringstream out;
            out << "ERR " << name << " value=" << a << " expected=" << b;
            return out.str();
        }
    }

    std::string RunWindEffectsSelfTest()
    {
        const VoxelGridDesc boxDesc = MakeDesc(64, 64, 64, -32.0f, -32.0f, -32.0f);
        WindDrivenParticles particles;

        // 1. Petal terminal velocity in still air: |g| * tau.
        const TestField still(boxDesc, 0.0f, NO_FLOOR);
        WindDrivenParticleSettings petal = MakeBaseSettings(WindParticleKind::Petal);
        petal.acceleration[1] = -9.8f;
        RunParticle(particles, still, petal, 0.0f, 2.0f);
        if (particles.Count() != 1)
            return Err("petalFallCount", static_cast<float>(particles.Count()), 1.0f);
        const float petalFall = particles.Velocity(0)[1];
        if (std::abs(petalFall - (-1.96f)) > 0.0196f)
            return Err("petalFall", petalFall, -1.96f);

        // 2. Drift: velocity relaxes to the wind.
        const TestField windy(boxDesc, 3.0f, NO_FLOOR);
        WindDrivenParticleSettings drifter = MakeBaseSettings(WindParticleKind::Petal);
        RunParticle(particles, windy, drifter, 0.0f, 2.0f);
        if (particles.Count() != 1)
            return Err("driftCount", static_cast<float>(particles.Count()), 1.0f);
        const float drift = particles.Velocity(0)[0];
        if (std::abs(drift - 3.0f) > 0.03f)
            return Err("drift", drift, 3.0f);

        // 3. Smoke rises.
        WindDrivenParticleSettings smoke = MakeBaseSettings(WindParticleKind::Smoke);
        smoke.buoyancy = 2.0f;
        smoke.dragTime = 0.5f;
        RunParticle(particles, still, smoke, 0.0f, 1.0f);
        if (particles.Count() != 1)
            return Err("smokeRiseCount", static_cast<float>(particles.Count()), 1.0f);
        const float smokeRise = particles.Velocity(0)[1];
        if (!(smokeRise > 0.0f) || !(particles.Position(0)[1] > 0.0f))
            return Err("smokeRise", smokeRise, 0.0f);

        // 4. Wall: a petal falling onto a floor at y = 0 stays above it.
        const TestField floorField(boxDesc, 0.0f, 0.0f);
        WindDrivenParticleSettings faller = MakeBaseSettings(WindParticleKind::Petal);
        faller.acceleration[1] = -9.8f;
        faller.wallFriction = 0.5f;
        RunParticle(particles, floorField, faller, 2.0f, 3.0f);
        if (particles.Count() != 1)
            return Err("wallCount", static_cast<float>(particles.Count()), 1.0f);
        const float wallY = particles.Position(0)[1];
        const uint64_t wallHits = particles.Stats().wallHits;
        if (wallY < -1e-3f)
            return Err("wallY", wallY, 0.0f);
        if (wallHits == 0)
            return Err("wallHits", 0.0f, 1.0f);

        // 5. Fire on a floor with wind 10 m/s along +x spreads farther downwind.
        const FireExtent windFire = RunFire(10.0f, 7u);
        if (!(windFire.down > windFire.up) || windFire.down < 2 * (std::max)(windFire.up, 1))
        {
            std::ostringstream out;
            out << "ERR fireWind down=" << windFire.down << " up=" << windFire.up << " side=" << windFire.side;
            return out.str();
        }

        // 6. Calm: symmetric spread.
        const FireExtent calm = RunFire(0.0f, 7u);
        const int calmMax = (std::max)(calm.down, calm.up);
        if (calm.down <= 0 || std::abs(calm.down - calm.up) > (std::max)(3.0f, 0.5f * static_cast<float>(calmMax)))
        {
            std::ostringstream out;
            out << "ERR fireCalm calmX+=" << calm.down << " calmX-=" << calm.up;
            return out.str();
        }

        // 7. maxSpreadRadius: with the same downwind run, no cell beyond 4 voxels of the ignition cell burns.
        const FireExtent capped = RunFire(10.0f, 7u, 4.0f);
        if (capped.radius > 4.0f || capped.down <= 0)
            return Err("fireRadius", capped.radius, 4.0f);

        // Surface cell count: the 41x41 floor layer.
        {
            const VoxelGridDesc desc = MakeDesc(41, 3, 41, 0.0f, 0.0f, 0.0f);
            OccupancyBitGrid grid(41, 3, 41);
            for (uint32_t z = 0; z < 41; ++z)
                for (uint32_t x = 0; x < 41; ++x)
                    grid.Set(x, 0, z);
            WindFireSpread fire;
            fire.Build(grid, desc, WindFireSettings(), 1u);
            if (fire.Stats().surfaceCells != 41u * 41u)
                return Err("surfaceCells", static_cast<float>(fire.Stats().surfaceCells), 41.0f * 41.0f);
        }

        // Flame colour: Helland's fit gives (255, 67.9, 0) at 1000 K and (255, 255, 255) at 6600 K, and the
        // flame ramp must cool monotonically from root to tip.
        {
            float rgb[3];
            BlackbodySrgb(1000.0f, rgb);
            if (rgb[0] != 255.0f || std::abs(rgb[1] - 67.9f) > 0.5f || rgb[2] != 0.0f)
                return Err("blackbody1000G", rgb[1], 67.9f);
            BlackbodySrgb(6600.0f, rgb);
            if (rgb[0] != 255.0f || rgb[1] != 255.0f || rgb[2] != 255.0f)
                return Err("blackbody6600B", rgb[2], 255.0f);
            if (!(FlameTemperature(0.0f) > FlameTemperature(0.5f) && FlameTemperature(0.5f) > FlameTemperature(1.0f)))
                return Err("flameRamp", FlameTemperature(0.5f), 0.0f);
        }

        std::ostringstream out;
        out << "OK petalFall=" << petalFall << " smokeRise=" << smokeRise << " drift=" << drift
            << " wallHits=" << wallHits << " fireDown=" << windFire.down << " fireUp=" << windFire.up
            << " fireSide=" << windFire.side << " calmX+=" << calm.down << " calmX-=" << calm.up
            << " cappedRadius=" << capped.radius;
        return out.str();
    }
}
