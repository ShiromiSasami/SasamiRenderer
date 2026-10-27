#include "AppFramework/Debug/RemoteControl/DebugFluidCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/StaticModel.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/IsoSurfaceRenderMesh.h"
#include "Renderer/Fluid/IsoSurfaceSelfTest.h"
#include "Renderer/Fluid/PbfIsoSurface.h"
#include "Renderer/Fluid/PbfIsoSurfaceSelfTest.h"
#include "Renderer/Fluid/PbfSelfTest.h"
#include "Renderer/Fluid/PbfSolver.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        std::optional<uint32_t> ParseUnsigned(const std::string& text)
        {
            try {
                size_t consumed = 0;
                const unsigned long value = std::stoul(text, &consumed);
                if (consumed != text.size() || text[0] == '-' || value > 0xFFFFFFFFul) {
                    return std::nullopt;
                }
                return static_cast<uint32_t>(value);
            }
            catch (...) {
                return std::nullopt;
            }
        }

        // The model `fluid.pbf mesh show` added. Raw pointer, so it is only trusted while it is still
        // in the app's static model list (the editor or a scene load may have deleted it).
        StaticModel* g_pbfMeshModel = nullptr;

        StaticModel* FindPbfMeshModel(const ApplicationCore& app)
        {
            if (!g_pbfMeshModel) {
                return nullptr;
            }
            const std::vector<StaticModel*> models = app.GetStaticModels();
            if (std::find(models.begin(), models.end(), g_pbfMeshModel) == models.end()) {
                g_pbfMeshModel = nullptr;
            }
            return g_pbfMeshModel;
        }

        bool RemovePbfMeshModel(ApplicationCore& app)
        {
            StaticModel* model = FindPbfMeshModel(app);
            if (!model) {
                return false;
            }
            app.DeleteObject(model);
            app.InvalidateRenderObjects();
            g_pbfMeshModel = nullptr;
            return true;
        }

        void SetupDamBreak(uint32_t particles, Render::Fluid::PbfParams& params, Render::Fluid::PbfSolver& solver)
        {
            // Particle radius is h / 4 (spacing h / 2) so a rest lattice has density ratio near 1.
            params.particleRadius = 0.025f;
            params.smoothingRadius = 0.1f;
            const float spacing = 2.0f * params.particleRadius;

            // Footprint is capped by the box; the block grows upward and the box grows with it.
            const uint32_t maxSide = static_cast<uint32_t>(std::floor((params.boxMax[0] - params.boxMin[0]) / spacing + 1e-3f));
            const uint32_t sideXZ = (std::min)(maxSide, static_cast<uint32_t>(std::ceil(std::cbrt(static_cast<double>(particles)))));
            const uint32_t layers = (particles + sideXZ * sideXZ - 1) / (sideXZ * sideXZ);
            const float blockSize[3] = { sideXZ * spacing, layers * spacing, sideXZ * spacing };
            params.boxMax[1] = (std::max)(params.boxMax[1], blockSize[1] + 0.5f);

            solver.Reset(params);
            const float lo[3] = { params.boxMin[0], params.boxMin[1], params.boxMin[2] };
            const float hi[3] = { lo[0] + blockSize[0], lo[1] + blockSize[1], lo[2] + blockSize[2] };
            solver.SpawnBlock(lo, hi);
        }

        double StepDamBreak(Render::Fluid::PbfSolver& solver, uint32_t steps)
        {
            double totalMs = 0.0;
            for (uint32_t step = 0; step < steps; ++step) {
                solver.Step(1.0f / 120.0f);
                totalMs += solver.LastStats().milliseconds;
            }
            return totalMs;
        }

        std::string RunDamBreak(uint32_t particles, uint32_t steps)
        {
            Render::Fluid::PbfParams params;
            Render::Fluid::PbfSolver solver;
            SetupDamBreak(particles, params, solver);
            const double totalMs = StepDamBreak(solver, steps);
            const Render::Fluid::PbfStepStats& stats = solver.LastStats();

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "OK particles=" << solver.ParticleCount()
                << " steps=" << steps
                << " meanDensity=" << stats.meanDensityRatio
                << " maxDensity=" << stats.maxDensityRatio
                << " maxNeighbors=" << stats.maxNeighbors
                << " msPerStep=" << (totalMs / steps)
                << " totalMs=" << totalMs;
            return out.str();
        }

        // Tail of `fluid.pbf mesh`: [aniso] [lambda=L] [nr=R] [taubin=K] [edge=E] [iso=F] [show [r g b] [at x,y,z]].
        // lambda/nr set AnisotropySettings::smoothing/neighbourScale, iso sets PbfSurfaceSettings::isoFraction.
        // False on anything malformed.
        bool ParseMeshTail(const std::vector<std::string>& args, Render::Fluid::PbfSurfaceSettings& surface,
                           bool& show, float tint[3], float offset[3])
        {
            size_t i = 3;
            if (i < args.size() && args[i] == "aniso") {
                surface.anisotropic = true;
                ++i;
            }
            for (; i < args.size() && args[i].find('=') != std::string::npos; ++i) {
                const std::string key = args[i].substr(0, args[i].find('='));
                const std::optional<float> value = ParseFloat(args[i].substr(args[i].find('=') + 1));
                if (!value) {
                    return false;
                }
                if (key == "lambda" && *value >= 0.0f && *value <= 1.0f) {
                    surface.anisotropy.smoothing = *value;
                } else if (key == "nr" && *value >= 0.5f && *value <= 4.0f) {
                    surface.anisotropy.neighbourScale = *value;
                } else if (key == "edge" && *value >= 0.0f && *value <= 16.0f) {
                    surface.anisotropy.edgeFade = *value;
                } else if (key == "taubin" && *value >= 0.0f && *value <= 20.0f) {
                    surface.anisotropy.taubinIterations = static_cast<uint32_t>(*value);
                } else if (key == "iso" && *value > 0.0f && *value < 1.0f) {
                    surface.isoFraction = *value;
                } else {
                    return false;
                }
            }
            if (i == args.size()) {
                return true;
            }
            if (args[i++] != "show") {
                return false;
            }
            show = true;
            if (i < args.size() && args[i] != "at") {
                if (i + 3 > args.size()) {
                    return false;
                }
                for (int c = 0; c < 3; ++c) {
                    const std::optional<float> value = ParseFloat(args[i++]);
                    if (!value || !(*value >= 0.0f && *value <= 1.0f)) {
                        return false;
                    }
                    tint[c] = *value;
                }
            }
            if (i == args.size()) {
                return true;
            }
            return args[i] == "at" && i + 2 == args.size() && ParseFloat3(args[i + 1], offset);
        }

        std::string RunMesh(ApplicationCore& app, uint32_t particles, uint32_t steps,
                            const Render::Fluid::PbfSurfaceSettings& surfaceSettings, bool show,
                            const float tint[3], const float offset[3])
        {
            Render::Fluid::PbfParams params;
            Render::Fluid::PbfSolver solver;
            SetupDamBreak(particles, params, solver);
            const double simMs = StepDamBreak(solver, steps);

            Render::Fluid::IsoSurfaceMesh mesh;
            Render::Fluid::PbfSurfaceStats stats;
            const bool anisotropic = surfaceSettings.anisotropic;
            Render::Fluid::BuildPbfIsoSurface(solver.Positions(), params.particleRadius, surfaceSettings, mesh, &stats);

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "OK particles=" << solver.ParticleCount()
                << " steps=" << steps
                << " tris=" << mesh.TriangleCount()
                << " verts=" << mesh.VertexCount()
                << " openEdges=" << Render::Fluid::CountOpenEdges(mesh)
                << " volume=" << Render::Fluid::IsoSurfaceVolume(mesh)
                << " iso=" << stats.iso
                << " cell=" << stats.cellSize
                << " dims=" << stats.dims[0] << "x" << stats.dims[1] << "x" << stats.dims[2]
                << " simMs=" << simMs
                << " splatMs=" << stats.splatMs
                << " meshMs=" << stats.meshMs
                << " aniso=" << (anisotropic ? 1 : 0);
            if (anisotropic) {
                out << " lambda=" << surfaceSettings.anisotropy.smoothing
                    << " nr=" << surfaceSettings.anisotropy.neighbourScale
                    << " taubin=" << surfaceSettings.anisotropy.taubinIterations
                    << " edge=" << surfaceSettings.anisotropy.edgeFade
                    << " anisoMs=" << stats.anisotropy.ms
                    << " ks=" << stats.anisotropy.stretchScale
                    << " isolated=" << stats.anisotropy.isolated
                    << " clamped=" << stats.anisotropy.clamped
                    << " maxExtent=" << stats.anisotropy.maxExtent;
            }
            if (show) {
                if (mesh.TriangleCount() == 0) {
                    return "ERR empty mesh, nothing to show";
                }
                const auto convertStart = std::chrono::steady_clock::now();
                for (size_t k = 0; k + 2 < mesh.positions.size(); k += 3) {
                    mesh.positions[k] += offset[0];
                    mesh.positions[k + 1] += offset[1];
                    mesh.positions[k + 2] += offset[2];
                }
                const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                Render::Mesh renderMesh = Render::Fluid::BuildIsoSurfaceRenderMesh(mesh, white);
                RemovePbfMeshModel(app);
                StaticModel* model = app.CreateStaticModel();
                if (!model) {
                    return "ERR could not create a static model";
                }
                model->AddStaticMesh(std::move(renderMesh),
                                     Render::MakeOpaqueSurfaceMaterial(tint[0], tint[1], tint[2], 0.2f, 0.0f));
                app.InvalidateRenderObjects();
                g_pbfMeshModel = model;
                const double convertMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - convertStart).count();
                out << " shown=1 convertMs=" << convertMs;
            }
            return out.str();
        }

        constexpr const char* PBF_USAGE =
            "ERR usage: fluid.pbf selftest | run <particles 1..200000> <steps 1..2000>"
            " | mesh <particles 1..200000> <steps 0..2000> [aniso] [lambda=L nr=R taubin=K edge=E iso=F] [show [r g b] [at x,y,z]] | hide | meshtest";
    }

    void RegisterFluidCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("fluid.pbf", "selftest | run <particles 1..200000> <steps 1..2000> | mesh <particles 1..200000> <steps 0..2000> [aniso] [lambda=L nr=R taubin=K edge=E iso=F] [show [r g b] [at x,y,z]] | hide | meshtest - CPU Position Based Fluids reference (Macklin & Mueller 2013); mesh = dam break surfaced with the particle iso-surface (aniso = Yu & Turk 2013 anisotropic kernels, show = add it to the scene as an opaque static model, hide = remove it); meshtest = CPU checks of the surfacing",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return PBF_USAGE;
                }
                if (args[0] == "selftest") {
                    return Render::Fluid::RunPbfSelfTest();
                }
                if (args[0] == "meshtest") {
                    return Render::Fluid::RunPbfIsoSurfaceSelfTest();
                }
                if (args[0] == "hide") {
                    return RemovePbfMeshModel(app) ? "OK hidden" : "OK nothing shown";
                }
                if (args[0] == "mesh") {
                    if (args.size() < 3) {
                        return PBF_USAGE;
                    }
                    const std::optional<uint32_t> particles = ParseUnsigned(args[1]);
                    if (!particles || *particles < 1 || *particles > 200000) {
                        return "ERR particles must be an integer in 1..200000";
                    }
                    const std::optional<uint32_t> steps = ParseUnsigned(args[2]);
                    if (!steps || *steps > 2000) {
                        return "ERR steps must be an integer in 0..2000";
                    }
                    Render::Fluid::PbfSurfaceSettings surfaceSettings;
                    bool show = false;
                    float tint[3] = { 0.25f, 0.55f, 0.85f };
                    float offset[3] = { 0.0f, 0.0f, 0.0f };
                    if (!ParseMeshTail(args, surfaceSettings, show, tint, offset)) {
                        return PBF_USAGE;
                    }
                    return RunMesh(app, *particles, *steps, surfaceSettings, show, tint, offset);
                }
                if (args[0] == "run") {
                    if (args.size() != 3) {
                        return PBF_USAGE;
                    }
                    const std::optional<uint32_t> particles = ParseUnsigned(args[1]);
                    if (!particles || *particles < 1 || *particles > 200000) {
                        return "ERR particles must be an integer in 1..200000";
                    }
                    const std::optional<uint32_t> steps = ParseUnsigned(args[2]);
                    if (!steps || *steps < 1 || *steps > 2000) {
                        return "ERR steps must be an integer in 1..2000";
                    }
                    return RunDamBreak(*particles, *steps);
                }
                return "ERR unknown subcommand '" + args[0] + "' (selftest|run|mesh|hide|meshtest)";
            });
        registry.Register("fluid.iso", "selftest - CPU particle splat + marching tetrahedra iso-surface checks (Zhu & Bridson 2005)",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty() && args[0] == "selftest") {
                    return Render::Fluid::RunIsoSurfaceSelfTest();
                }
                return "ERR usage: fluid.iso selftest";
            });
    }
}
