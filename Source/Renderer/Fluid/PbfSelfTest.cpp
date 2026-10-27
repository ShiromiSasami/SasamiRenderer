#include "Renderer/Fluid/PbfSelfTest.h"

#include "Renderer/Fluid/PbfSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace SasamiEngine::Render::Fluid
{
    namespace
    {
        // Fixed LCG so the particle sets are the same on every run.
        float NextUnit(uint32_t& state)
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
        }

        // Particle radius is h / 4 (spacing h / 2) so a rest lattice has density ratio near 1.
        PbfParams MakeTestParams()
        {
            PbfParams params;
            params.particleRadius = 0.025f;
            params.smoothingRadius = 0.1f;
            return params;
        }

        double IntegratePoly6(float h)
        {
            constexpr int N = 64;
            const double cell = 2.0 * h / N;
            double sum = 0.0;
            for (int z = 0; z < N; ++z) {
                for (int y = 0; y < N; ++y) {
                    for (int x = 0; x < N; ++x) {
                        const float px = static_cast<float>((x + 0.5) * cell - h);
                        const float py = static_cast<float>((y + 0.5) * cell - h);
                        const float pz = static_cast<float>((z + 0.5) * cell - h);
                        sum += Poly6Kernel(px * px + py * py + pz * pz, h);
                    }
                }
            }
            return sum * cell * cell * cell;
        }

        bool AllFiniteInside(const std::vector<float>& values, const float (&boxMin)[3], const float (&boxMax)[3], bool checkBox)
        {
            for (size_t i = 0; i < values.size(); ++i) {
                if (!std::isfinite(values[i])) {
                    return false;
                }
                const size_t axis = i % 3;
                if (checkBox && (values[i] < boxMin[axis] || values[i] > boxMax[axis])) {
                    return false;
                }
            }
            return true;
        }

        void SetupDamBreak(PbfSolver& solver)
        {
            solver.Reset(MakeTestParams());
            const float lo[3] = { 0.0f, 0.0f, 0.0f };
            const float hi[3] = { 0.5f, 0.5f, 0.5f };
            solver.SpawnBlock(lo, hi);
        }
    }

    std::string RunPbfSelfTest()
    {
        int checks = 0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(4);

        // (a) Poly6 integrates to 1 over its support.
        const double integral = IntegratePoly6(0.1f);
        ++checks;
        if (std::abs(integral - 1.0) > 0.02) {
            out << "ERR poly6 integral=" << integral << " (expected 1 +-0.02)";
            return out.str();
        }

        // (b) Grid neighbor lists equal brute force as sorted sets.
        {
            PbfSolver solver;
            PbfParams params = MakeTestParams();
            params.boxMax[0] = params.boxMax[1] = params.boxMax[2] = 1.0f;
            solver.Reset(params);
            std::vector<float> positions(2000 * 3);
            uint32_t state = 12345u;
            for (float& value : positions) {
                value = NextUnit(state);
            }
            solver.BuildNeighbors(positions);
            std::vector<std::vector<uint32_t>> reference;
            FindNeighborsBruteForce(positions, params.smoothingRadius, reference);
            ++checks;
            const std::vector<std::vector<uint32_t>>& grid = solver.LastNeighbors();
            if (grid.size() != reference.size()) {
                out << "ERR neighbor list count " << grid.size() << " != " << reference.size();
                return out.str();
            }
            for (size_t i = 0; i < grid.size(); ++i) {
                std::vector<uint32_t> sorted = grid[i];
                std::sort(sorted.begin(), sorted.end());
                if (sorted != reference[i]) {
                    out << "ERR neighbor mismatch at particle " << i << " grid=" << sorted.size() << " brute=" << reference[i].size();
                    return out.str();
                }
            }
        }

        // (c) Dam break stays finite, inside the box and near rest density.
        PbfStepStats lastStats;
        double totalMs = 0.0;
        constexpr int DAM_STEPS = 120;
        {
            PbfSolver solver;
            SetupDamBreak(solver);
            for (int step = 0; step < DAM_STEPS; ++step) {
                solver.Step(1.0f / 120.0f);
                totalMs += solver.LastStats().milliseconds;
            }
            lastStats = solver.LastStats();
            const PbfParams& params = solver.Params();
            ++checks;
            if (!AllFiniteInside(solver.Positions(), params.boxMin, params.boxMax, true) ||
                !AllFiniteInside(solver.Velocities(), params.boxMin, params.boxMax, false)) {
                out << "ERR dam break has NaN/Inf or particles outside the box";
                return out.str();
            }
            ++checks;
            if (!(lastStats.meanDensityRatio >= 0.8f && lastStats.meanDensityRatio <= 1.2f)) {
                out << "ERR dam break meanDensityRatio=" << lastStats.meanDensityRatio << " (expected 0.8..1.2)";
                return out.str();
            }
        }

        // (d) Two independent solvers are bit-identical.
        {
            PbfSolver a;
            PbfSolver b;
            SetupDamBreak(a);
            SetupDamBreak(b);
            for (int step = 0; step < 30; ++step) {
                a.Step(1.0f / 120.0f);
                b.Step(1.0f / 120.0f);
            }
            ++checks;
            const bool sameSize = a.Positions().size() == b.Positions().size() && a.Velocities().size() == b.Velocities().size();
            if (!sameSize ||
                std::memcmp(a.Positions().data(), b.Positions().data(), a.Positions().size() * sizeof(float)) != 0 ||
                std::memcmp(a.Velocities().data(), b.Velocities().data(), a.Velocities().size() * sizeof(float)) != 0) {
                out << "ERR determinism: two solvers diverged after 30 steps";
                return out.str();
            }
        }

        out << "OK checks=" << checks
            << " poly6Integral=" << integral
            << " damBreakMeanDensity=" << lastStats.meanDensityRatio
            << " damBreakMaxDensity=" << lastStats.maxDensityRatio
            << " maxNeighbors=" << lastStats.maxNeighbors
            << " msPerStep=" << (totalMs / DAM_STEPS);
        return out.str();
    }
}
