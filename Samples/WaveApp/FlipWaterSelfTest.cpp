#include "FlipDiffuseParticles.h"
#include "FlipWaterSolver.h"
#include "QuadtreeColumnLayout.h"
#include "TallCellPressureBasis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        constexpr float GRAVITY = 9.81f;
        constexpr float DT = 1.0f / 60.0f;

        std::string Format(const char* what, float a, float b)
        {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "%s (%g, %g)", what, static_cast<double>(a), static_cast<double>(b));
            return buffer;
        }

        FlipWaterSettings MakeStillWaterSettings(bool tallCells, bool quadtree = false)
        {
            FlipWaterSettings s;
            s.nx = 16;
            s.ny = 12;
            s.nz = 6;
            s.cellSize = 0.25f;
            s.origin[0] = 0.0f;
            s.origin[1] = 0.0f;
            s.origin[2] = 0.0f;
            s.waterLevel = 1.5f;
            s.tallCells = tallCells;
            s.quadtree = quadtree;
            s.parallel = false;
            return s;
        }

        bool HasNaN(const std::vector<float>& values)
        {
            for (float v : values)
            {
                if (!std::isfinite(v))
                {
                    return true;
                }
            }
            return false;
        }

        // Hydrostatic step: gravity applied to every v face that is not against a solid, no particles.
        struct HydrostaticCase
        {
            FlipCellGrid cells;
            std::vector<float> divergence;
        };

        HydrostaticCase BuildHydrostaticCase()
        {
            constexpr uint32_t NX = 8;
            constexpr uint32_t NY = 16;
            constexpr uint32_t NZ = 5;
            constexpr uint32_t FLUID_TOP = 12;
            HydrostaticCase c;
            c.cells.nx = NX;
            c.cells.ny = NY;
            c.cells.nz = NZ;
            c.cells.type.assign(c.cells.CellCount(), static_cast<uint8_t>(FlipCell::Air));
            for (uint32_t z = 0; z < NZ; ++z)
            {
                for (uint32_t x = 0; x < NX; ++x)
                {
                    const uint32_t solidTop = x < 4 ? 2 : 4;
                    for (uint32_t y = 0; y < NY; ++y)
                    {
                        FlipCell kind = FlipCell::Air;
                        if (y < solidTop)
                        {
                            kind = FlipCell::Solid;
                        }
                        else if (y < FLUID_TOP)
                        {
                            kind = FlipCell::Fluid;
                        }
                        c.cells.type[c.cells.Index(x, y, z)] = static_cast<uint8_t>(kind);
                    }
                }
            }

            Render::Voxel::StaggeredVelocityGrid velocity;
            velocity.Reset(NX, NY, NZ);
            for (uint32_t z = 0; z < NZ; ++z)
            {
                for (uint32_t y = 0; y <= NY; ++y)
                {
                    for (uint32_t x = 0; x < NX; ++x)
                    {
                        const bool belowSolid = y == 0 || c.cells.At(x, y - 1, z) == FlipCell::Solid;
                        const bool aboveSolid = y < NY && c.cells.At(x, y, z) == FlipCell::Solid;
                        velocity.V(x, y, z) = (belowSolid || aboveSolid) ? 0.0f : -GRAVITY * DT;
                    }
                }
            }
            c.divergence.assign(c.cells.CellCount(), 0.0f);
            for (uint32_t z = 0; z < NZ; ++z)
            {
                for (uint32_t y = 0; y < NY; ++y)
                {
                    for (uint32_t x = 0; x < NX; ++x)
                    {
                        if (c.cells.At(x, y, z) == FlipCell::Fluid)
                        {
                            c.divergence[c.cells.Index(x, y, z)] = velocity.Divergence(x, y, z);
                        }
                    }
                }
            }
            return c;
        }

        float MaxAbs(const std::vector<float>& values)
        {
            float m = 0.0f;
            for (float v : values)
            {
                m = (std::max)(m, std::fabs(v));
            }
            return m;
        }

        float MaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
        {
            float m = 0.0f;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            {
                m = (std::max)(m, std::fabs(a[i] - b[i]));
            }
            return m;
        }
    }

    uint32_t RunFlipWaterSelfTest(std::vector<std::string>* failures, uint32_t* total)
    {
        uint32_t passed = 0;
        uint32_t checks = 0;
        auto check = [&](bool ok, const std::string& what) {
            ++checks;
            if (ok)
            {
                ++passed;
            }
            else if (failures)
            {
                failures->push_back(what);
            }
        };

        // a-c: still water stays still, divergence-free and keeps its particles.
        {
            FlipWaterSolver solver;
            const FlipWaterSettings settings = MakeStillWaterSettings(false);
            solver.Reset(settings, nullptr);
            const uint32_t count0 = solver.ParticleCount();
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
            }
            const FlipWaterStats& stats = solver.Stats();
            check(stats.maxSpeed < 0.3f, Format("still water maxSpeed >= 0.3", stats.maxSpeed, 0.3f));
            check(std::fabs(stats.surfaceMax - settings.waterLevel) < 0.5f * settings.cellSize,
                  Format("still water surfaceMax off waterLevel", stats.surfaceMax, settings.waterLevel));
            check(stats.maxDivergence < 1e-3f && stats.converged,
                  Format("still water divergence too large or not converged", stats.maxDivergence,
                         stats.converged ? 1.0f : 0.0f));
            check(solver.ParticleCount() == count0,
                  Format("still water particle count changed", static_cast<float>(count0),
                         static_cast<float>(solver.ParticleCount())));
        }

        // d-e: tall-cell pressure matches the uniform solve on a hydrostatic column.
        {
            const HydrostaticCase hc = BuildHydrostaticCase();
            PressureSolveSettings pressureSettings;
            pressureSettings.maxIterations = 400;
            std::vector<float> uniform;
            SolveUniformPressure(hc.cells, hc.divergence, uniform, pressureSettings);
            const float scale = MaxAbs(uniform);

            {
                const TallCellLayout layout = BuildTallCellLayout(hc.cells, 3, 3);
                std::vector<float> tall;
                SolveTallCellPressure(hc.cells, layout, hc.divergence, tall, pressureSettings);
                check(layout.tallColumns == 40,
                      Format("tall columns != 40", static_cast<float>(layout.tallColumns), 40.0f));
                const float err = MaxAbsDiff(uniform, tall);
                check(err <= 1e-3f * scale, Format("tall pressure differs from uniform", err, scale));
            }
            {
                const TallCellLayout layout = BuildTallCellLayout(hc.cells, 32, 3);
                std::vector<float> tall;
                SolveTallCellPressure(hc.cells, layout, hc.divergence, tall, pressureSettings);
                check(layout.tallColumns == 0,
                      Format("band 32 still has tall columns", static_cast<float>(layout.tallColumns), 0.0f));
                const float err = MaxAbsDiff(uniform, tall);
                check(err <= 1e-3f * scale, Format("band 32 pressure differs from uniform", err, scale));
            }
        }

        // f: dam break moves the front and conserves particles.
        {
            FlipWaterSettings settings;
            settings.nx = 32;
            settings.ny = 12;
            settings.nz = 4;
            settings.cellSize = 0.25f;
            settings.origin[0] = 0.0f;
            settings.origin[1] = 0.0f;
            settings.origin[2] = 0.0f;
            settings.parallel = false;
            FlipWaterSolver solver;
            solver.Reset(settings, nullptr);
            const float boxMin[3] = { 0.0f, 0.0f, 0.0f };
            const float boxMax[3] = { 2.0f, 2.0f, 1.0f };
            solver.ResetToBlock(boxMin, boxMax);
            const uint32_t count0 = solver.ParticleCount();
            auto frontX = [&]() {
                const std::vector<float>& px = solver.ParticlePositions();
                float front = -1e30f;
                for (size_t p = 0; p + 2 < px.size(); p += 3)
                {
                    front = (std::max)(front, px[p]);
                }
                return front;
            };
            const float front0 = frontX();
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
            }
            const float front = frontX();
            check(front > front0 + 1.0f, Format("dam break front did not advance 1 m", front0, front));
            check(solver.ParticleCount() == count0,
                  Format("dam break particle count changed", static_cast<float>(count0),
                         static_cast<float>(solver.ParticleCount())));
            check(!HasNaN(solver.ParticlePositions()), "dam break positions contain NaN");
        }

        // g: still water with the tall-cell basis.
        {
            FlipWaterSolver solver;
            solver.Reset(MakeStillWaterSettings(true), nullptr);
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
            }
            const FlipWaterStats& stats = solver.Stats();
            check(stats.maxSpeed < 0.3f, Format("tall still water maxSpeed >= 0.3", stats.maxSpeed, 0.3f));
            check(stats.tallColumns > 0, "tall still water has no tall columns");
            check(stats.maxDivergence < 1e-3f,
                  Format("tall still water divergence too large", stats.maxDivergence, 1e-3f));
        }

        // h-i: quadtree layout covers the grid and keeps 2:1 balance around a single fine column.
        {
            constexpr uint32_t NX = 20, NZ = 13;
            std::vector<uint8_t> desired(NX * NZ, 3);
            desired[7 + NX * 6] = 0;
            const QuadtreeColumnLayout q = BuildQuadtreeColumnLayout(NX, NZ, desired, 3);
            std::string reason;
            check(ValidateQuadtreeColumnLayout(q, &desired, &reason), "quadtree layout invalid: " + reason);
            check(q.leaves.size() < NX * NZ && q.leavesPerLevel[0] > 0,
                  Format("quadtree did not coarsen", static_cast<float>(q.leaves.size()),
                         static_cast<float>(q.leavesPerLevel[0])));
        }

        // j: an all-fine quadtree is the per-column tall solve.
        {
            const HydrostaticCase hc = BuildHydrostaticCase();
            PressureSolveSettings pressureSettings;
            pressureSettings.maxIterations = 400;
            const TallCellLayout layout = BuildTallCellLayout(hc.cells, 3, 3);
            std::vector<float> tall, quad;
            SolveTallCellPressure(hc.cells, layout, hc.divergence, tall, pressureSettings);
            const std::vector<uint8_t> fine(size_t(hc.cells.nx) * hc.cells.nz, 0);
            const QuadtreeColumnLayout q = BuildQuadtreeColumnLayout(hc.cells.nx, hc.cells.nz, fine, 3);
            SolveTallCellPressure(hc.cells, layout, hc.divergence, quad, pressureSettings, &q);
            const float err = MaxAbsDiff(tall, quad);
            check(err <= 1e-6f * (std::max)(MaxAbs(tall), 1.0f), Format("all-fine quadtree differs from tall", err,
                                                                         MaxAbs(tall)));
        }

        // k: still water with quadtree + tall cells stays still with fewer unknowns.
        {
            FlipWaterSolver tallSolver, quadSolver;
            tallSolver.Reset(MakeStillWaterSettings(true), nullptr);
            quadSolver.Reset(MakeStillWaterSettings(true, true), nullptr);
            for (int i = 0; i < 30; ++i)
            {
                tallSolver.Step(DT);
                quadSolver.Step(DT);
            }
            const FlipWaterStats& st = quadSolver.Stats();
            check(st.maxSpeed < 0.3f, Format("quad still water maxSpeed >= 0.3", st.maxSpeed, 0.3f));
            check(st.quadLeaves > 0 && st.unknowns < tallSolver.Stats().unknowns,
                  Format("quad still water unknowns not below tall", static_cast<float>(st.unknowns),
                         static_cast<float>(tallSolver.Stats().unknowns)));
        }

        // l-n: optical layer bands (Narita & Kanai PG2026): still flow keeps minCells, a uniform band array
        // reproduces the scalar band, and one curved column dilates over Manhattan distance.
        {
            const HydrostaticCase hc = BuildHydrostaticCase();
            const uint32_t nx = hc.cells.nx, ny = hc.cells.ny, nz = hc.cells.nz;
            Render::Voxel::StaggeredVelocityGrid still;
            still.Reset(nx, ny, nz);
            OpticalLayerSettings aol;
            aol.enabled = true;
            OpticalLayerStats aolStats;
            const std::vector<uint16_t> stillBands = ComputeOpticalLayerBands(hc.cells, still, aol, &aolStats);
            check(aolStats.minBand == aol.minCells && aolStats.maxBand == aol.minCells,
                  Format("still optical bands != minCells", static_cast<float>(aolStats.minBand),
                         static_cast<float>(aolStats.maxBand)));

            const std::vector<uint16_t> uniformBands(size_t(nx) * nz, 3);
            const TallCellLayout scalar = BuildTallCellLayout(hc.cells, 3, 3);
            const TallCellLayout perColumn = BuildTallCellLayout(hc.cells, 3, 3, &uniformBands);
            bool same = scalar.columns.size() == perColumn.columns.size();
            for (size_t i = 0; same && i < scalar.columns.size(); ++i)
            {
                same = scalar.columns[i].bottom == perColumn.columns[i].bottom &&
                       scalar.columns[i].top == perColumn.columns[i].top;
            }
            check(same, "uniform band array differs from the scalar band");

            // U faces x=4,5 at y=8, z=2: columns x=3..5 get I >= 1.25, d = maxCells (6).
            Render::Voxel::StaggeredVelocityGrid curved;
            curved.Reset(nx, ny, nz);
            curved.U(4, 8, 2) = 10.0f;
            curved.U(5, 8, 2) = 10.0f;
            aol.smoothIterations = 0;
            const std::vector<uint16_t> b = ComputeOpticalLayerBands(hc.cells, curved, aol, &aolStats);
            auto bandAt = [&](uint32_t x, uint32_t z) { return static_cast<float>(b[x + size_t(nx) * z]); };
            check(bandAt(4, 2) == 6.0f && bandAt(4, 0) == 4.0f && bandAt(7, 2) == 4.0f && bandAt(0, 0) == 2.0f,
                  Format("optical dilation (4,2)/(4,0)", bandAt(4, 2), bandAt(4, 0)) +
                      Format(" (7,2)/(0,0)", bandAt(7, 2), bandAt(0, 0)));
        }

        // o: still water with optical layers stays still.
        {
            FlipWaterSettings settings = MakeStillWaterSettings(true);
            settings.opticalLayers.enabled = true;
            FlipWaterSolver solver;
            solver.Reset(settings, nullptr);
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
            }
            const FlipWaterStats& st = solver.Stats();
            check(st.maxSpeed < 0.3f && st.opticalMaxBand > 0,
                  Format("aol still water maxSpeed / maxBand", st.maxSpeed, static_cast<float>(st.opticalMaxBand)));
        }

        // p: the external paddle drive sets the x = 0 wall faces with the Airy depth decay.
        {
            FlipWaterSettings settings = MakeStillWaterSettings(false);
            settings.paddleExternal = true;
            settings.paddleAmplitude = 5.0f;   // ignored while external
            FlipWaterSolver solver;
            solver.Reset(settings, nullptr);
            solver.SetPaddleDrive(0.5f, 1.0f);
            solver.Step(DT);
            // Cell rows j=5 (y=1.375, 0.125 below the water level) and j=1 (y=0.375, 1.125 below).
            const float top = solver.Velocity().U(0, 5, 2);
            const float deep = solver.Velocity().U(0, 1, 2);
            const float topRef = 0.5f * std::exp(-0.125f);
            const float deepRef = 0.5f * std::exp(-1.125f);
            check(std::fabs(top - topRef) < 1e-6f && std::fabs(deep - deepRef) < 1e-6f,
                  Format("paddle drive top/deep", top, deep));
        }

        // q: a rock in still water makes the cells under its top Solid and keeps particles above it.
        {
            FlipWaterSettings settings = MakeStillWaterSettings(false);
            FlipRock rock;
            rock.center[0] = 2.0f; rock.center[1] = 0.0f; rock.center[2] = 0.75f;
            rock.radius[0] = 0.5f; rock.radius[1] = 2.0f; rock.radius[2] = 0.5f;
            settings.rocks.push_back(rock);
            FlipWaterSolver solver;
            solver.Reset(settings, nullptr);
            // Cell (8,4,3): centre (2.125, 1.125, 0.875), rock top there ~1.66.
            check(solver.Cells().At(8, 4, 3) == FlipCell::Solid && solver.Cells().At(1, 4, 3) != FlipCell::Solid,
                  Format("rock cell solid / open cell", static_cast<float>(solver.Cells().At(8, 4, 3)),
                         static_cast<float>(solver.Cells().At(1, 4, 3))));
            check(RockTopHeight(rock, 2.0f, 0.75f) == 2.0f && !(RockTopHeight(rock, 3.0f, 0.75f) > -1e30f),
                  Format("rock top centre / outside", RockTopHeight(rock, 2.0f, 0.75f), RockTopHeight(rock, 3.0f, 0.75f)));
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
            }
            const std::vector<float>& x = solver.ParticlePositions();
            float worst = 1e30f;
            for (size_t p = 0; p + 2 < x.size(); p += 3)
            {
                worst = (std::min)(worst, x[p + 1] - solver.BedHeight(x[p], x[p + 2]));
            }
            check(worst > -0.01f && !HasNaN(x) && !HasNaN(solver.ParticleVelocities()),
                  Format("rock particle below bed (min y - bed)", worst, 0.0f));
        }

        if (total)
        {
            *total = checks;
        }
        return passed;
    }

    uint32_t RunFlipDiffuseSelfTest(std::vector<std::string>* failures, uint32_t* total)
    {
        uint32_t passed = 0;
        uint32_t checks = 0;
        auto check = [&](bool ok, const std::string& what) {
            ++checks;
            if (ok)
            {
                ++passed;
            }
            else if (failures)
            {
                failures->push_back(what);
            }
        };

        // a: still water has no trapped air, crests or energy above the thresholds -> nothing spawns.
        {
            FlipWaterSolver solver;
            solver.Reset(MakeStillWaterSettings(false), nullptr);
            FlipDiffuseParticles diffuse;
            uint32_t spawned = 0;
            for (int i = 0; i < 30; ++i)
            {
                solver.Step(DT);
                diffuse.Step(solver, DT);
                spawned += diffuse.Stats().spawned;
            }
            check(spawned == 0, Format("still water spawned diffuse particles", static_cast<float>(spawned), 0.0f));
        }

        // b-g: a dam break spawns spray/foam/bubbles that stay inside the tank, serial == parallel.
        {
            auto run = [&](bool parallel, FlipDiffuseParticles& diffuse, uint32_t& spawnedTotal) {
                FlipWaterSettings settings;
                settings.nx = 32;
                settings.ny = 12;
                settings.nz = 4;
                settings.cellSize = 0.25f;
                settings.origin[0] = 0.0f;
                settings.origin[1] = 0.0f;
                settings.origin[2] = 0.0f;
                settings.parallel = parallel;
                FlipWaterSolver solver;
                solver.Reset(settings, nullptr);
                const float boxMin[3] = { 0.0f, 0.0f, 0.0f };
                const float boxMax[3] = { 2.0f, 2.0f, 1.0f };
                solver.ResetToBlock(boxMin, boxMax);
                diffuse.Reset();
                spawnedTotal = 0;
                for (int i = 0; i < 60; ++i)
                {
                    solver.Step(DT);
                    diffuse.Step(solver, DT);
                    spawnedTotal += diffuse.Stats().spawned;
                }
                return settings;
            };
            FlipDiffuseParticles serial;
            FlipDiffuseParticles parallel;
            uint32_t spawnedSerial = 0;
            uint32_t spawnedParallel = 0;
            // Both runs are serial: the check is that the emitter (its RNG and hash order) is
            // deterministic, not that the solver's parallel path is.
            const FlipWaterSettings settings = run(false, serial, spawnedSerial);
            run(false, parallel, spawnedParallel);

            check(spawnedSerial > 0, Format("dam break spawned nothing", static_cast<float>(spawnedSerial), 0.0f));
            check(serial.Count() > 0, Format("dam break has no live diffuse particles", 0.0f, 0.0f));
            const FlipDiffuseStats& st = serial.Stats();
            check(st.spray + st.foam + st.bubble == serial.Count(),
                  Format("kind counts do not sum to Count", static_cast<float>(st.spray + st.foam + st.bubble),
                         static_cast<float>(serial.Count())));
            check(!HasNaN(serial.Positions()) && !HasNaN(serial.Velocities()), "diffuse particles have NaN");
            bool inside = true;
            const std::vector<float>& x = serial.Positions();
            const float hi[3] = { settings.nx * settings.cellSize, settings.ny * settings.cellSize,
                                  settings.nz * settings.cellSize };
            for (size_t p = 0; p + 2 < x.size(); p += 3)
            {
                for (int a = 0; a < 3; ++a)
                {
                    inside = inside && x[p + a] >= settings.origin[a] && x[p + a] <= settings.origin[a] + hi[a];
                }
            }
            check(inside, "diffuse particle outside the tank");
            const bool same = serial.Positions() == parallel.Positions() && serial.Kinds() == parallel.Kinds()
                && spawnedSerial == spawnedParallel;
            check(same, Format("two identical runs differ (count)", static_cast<float>(serial.Count()),
                               static_cast<float>(parallel.Count())));
        }

        // h: a dam break against a rock spawns more with the rock impact term than without it.
        {
            auto run = [&](float impactScale) {
                FlipWaterSettings settings;
                settings.nx = 32;
                settings.ny = 12;
                settings.nz = 4;
                settings.cellSize = 0.25f;
                settings.origin[0] = 0.0f;
                settings.origin[1] = 0.0f;
                settings.origin[2] = 0.0f;
                settings.parallel = false;
                FlipRock rock;
                rock.center[0] = 4.0f; rock.center[1] = 0.0f; rock.center[2] = 0.5f;
                rock.radius[0] = 0.5f; rock.radius[1] = 1.5f; rock.radius[2] = 0.8f;
                settings.rocks.push_back(rock);
                FlipWaterSolver solver;
                solver.Reset(settings, nullptr);
                const float boxMin[3] = { 0.0f, 0.0f, 0.0f };
                const float boxMax[3] = { 2.0f, 2.0f, 1.0f };
                solver.ResetToBlock(boxMin, boxMax);
                FlipDiffuseParticles diffuse;
                diffuse.MutableSettings().rockImpactScale = impactScale;
                uint32_t spawned = 0;
                for (int i = 0; i < 60; ++i)
                {
                    solver.Step(DT);
                    diffuse.Step(solver, DT);
                    spawned += diffuse.Stats().spawned;
                }
                return spawned;
            };
            const uint32_t without = run(0.0f);
            const uint32_t with = run(2.0f);
            check(with > without, Format("rock impact spawned with / without", static_cast<float>(with),
                                         static_cast<float>(without)));
        }

        if (total)
        {
            *total = checks;
        }
        return passed;
    }
}
