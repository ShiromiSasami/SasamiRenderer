#include "AppFramework/Debug/RemoteControl/DebugSmokeMeshCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/Debug/SmokeSurfaceOverlay.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/StaticModel.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/IsoSurfaceRenderMesh.h"
#include "Renderer/Fluid/ParticleSplatGrid.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // The model `smoke.mesh show` added. Raw pointer, trusted only while it is still in the
        // app's static model list (the editor or a scene load may have deleted it).
        StaticModel* g_smokeMeshModel = nullptr;

        StaticModel* FindSmokeMeshModel(const ApplicationCore& app)
        {
            if (!g_smokeMeshModel) {
                return nullptr;
            }
            const std::vector<StaticModel*> models = app.GetStaticModels();
            if (std::find(models.begin(), models.end(), g_smokeMeshModel) == models.end()) {
                g_smokeMeshModel = nullptr;
            }
            return g_smokeMeshModel;
        }

        bool RemoveSmokeMeshModel(ApplicationCore& app)
        {
            StaticModel* model = FindSmokeMeshModel(app);
            if (!model) {
                return false;
            }
            app.DeleteObject(model);
            app.InvalidateRenderObjects();
            g_smokeMeshModel = nullptr;
            return true;
        }

        double MsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    }

    void RegisterSmokeMeshCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("smoke.mesh", "[iso] [show [r g b]] | hide | live on [iso] | live off | live status - read the GPU smoke density back (waits for the GPU) and mesh its iso level (default 0.5) with marching tetrahedra; reports density range, triangles, open edges, volume and timings; show = also add the mesh to the scene as an opaque static model (a snapshot, replacing the previous one), hide = remove it. live = re-mesh every frame from a non-blocking readback ring (~3 frames late) into a fixed-capacity scene mesh (bounds = the smoke box at 'on'). DX12-only like the smoke sim",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: smoke.mesh [iso > 0] [show [r g b]] | hide | live on [iso] | live off | live status";
                if (!args.empty() && args[0] == "live") {
                    SmokeSurfaceOverlay& live = app.GetSmokeSurfaceOverlay();
                    if (args.size() == 2 && args[1] == "off") {
                        live.Disable(app);
                        return "OK live=0";
                    }
                    if (args.size() == 2 && args[1] == "status") {
                        return live.Status();
                    }
                    if ((args.size() == 2 || args.size() == 3) && args[1] == "on") {
                        float liveIso = 0.5f;
                        if (args.size() == 3) {
                            const auto value = ParseFloat(args[2]);
                            if (!value || !(*value > 0.0f)) {
                                return USAGE;
                            }
                            liveIso = *value;
                        }
                        return live.Enable(app, liveIso);
                    }
                    return USAGE;
                }
                if (args.size() == 1 && args[0] == "hide") {
                    return RemoveSmokeMeshModel(app) ? "OK hidden" : "OK nothing shown";
                }
                size_t i = 0;
                float iso = 0.5f;
                if (i < args.size() && args[i] != "show") {
                    const auto value = ParseFloat(args[i]);
                    if (!value || !(*value > 0.0f)) {
                        return USAGE;
                    }
                    iso = *value;
                    ++i;
                }
                bool show = false;
                // Pale grey by eye, so the plume reads apart from the scene.
                float tint[3] = { 0.78f, 0.78f, 0.80f };
                if (i < args.size() && args[i] == "show") {
                    show = true;
                    ++i;
                    if (i + 3 == args.size()) {
                        for (int c = 0; c < 3; ++c) {
                            const auto value = ParseFloat(args[i + size_t(c)]);
                            if (!value || *value < 0.0f || *value > 1.0f) {
                                return USAGE;
                            }
                            tint[c] = *value;
                        }
                        i += 3;
                    }
                }
                if (i != args.size()) {
                    return USAGE;
                }

                Renderer& renderer = app.GetRenderer();
                SmokeVolumeSim& sim = renderer.GetSmokeSimMutable();
                const auto readStart = std::chrono::steady_clock::now();
                std::vector<float> density;
                if (!sim.ReadbackDensity(density)) {
                    return "ERR smoke density readback failed (smoke sim not running / not allocated?)";
                }
                const double readMs = MsSince(readStart);

                Render::Fluid::SplatGridDesc desc;
                desc.origin[0] = sim.GetOriginX();
                desc.origin[1] = sim.GetOriginY();
                desc.origin[2] = sim.GetOriginZ();
                desc.cellSize = sim.GetCellSize();
                desc.dims[0] = sim.GetCountX();
                desc.dims[1] = sim.GetCountY();
                desc.dims[2] = sim.GetCountZ();

                float minDensity = density.empty() ? 0.0f : density[0];
                float maxDensity = minDensity;
                uint32_t above = 0;
                for (const float d : density) {
                    minDensity = (std::min)(minDensity, d);
                    maxDensity = (std::max)(maxDensity, d);
                    above += d > iso ? 1u : 0u;
                }

                const auto meshStart = std::chrono::steady_clock::now();
                Render::Fluid::IsoSurfaceMesh mesh;
                // Outside the box counts as empty air, so a plume touching a wall is capped.
                Render::Fluid::ExtractIsoSurface(desc, density.data(), iso, 0.0f, mesh);
                const double meshMs = MsSince(meshStart);

                std::ostringstream out;
                out << std::fixed << std::setprecision(3);
                out << "OK dims=" << desc.dims[0] << "x" << desc.dims[1] << "x" << desc.dims[2]
                    << " cell=" << desc.cellSize
                    << " iso=" << iso
                    << " density=" << minDensity << ".." << maxDensity
                    << " cellsAbove=" << above
                    << " tris=" << mesh.TriangleCount()
                    << " openEdges=" << Render::Fluid::CountOpenEdges(mesh)
                    << " volume=" << Render::Fluid::IsoSurfaceVolume(mesh)
                    << " readMs=" << readMs << " meshMs=" << meshMs;
                if (show) {
                    if (mesh.TriangleCount() == 0) {
                        return out.str() + " ERR empty mesh, nothing to show";
                    }
                    const auto convertStart = std::chrono::steady_clock::now();
                    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    Render::Mesh renderMesh = Render::Fluid::BuildIsoSurfaceRenderMesh(mesh, white);
                    RemoveSmokeMeshModel(app);
                    StaticModel* model = app.CreateStaticModel();
                    if (!model) {
                        return "ERR could not create a static model";
                    }
                    model->AddStaticMesh(std::move(renderMesh),
                                         Render::MakeOpaqueSurfaceMaterial(tint[0], tint[1], tint[2], 0.9f, 0.0f));
                    app.InvalidateRenderObjects();
                    g_smokeMeshModel = model;
                    out << " shown=1 convertMs=" << MsSince(convertStart);
                }
                return out.str();
            });
    }
}
