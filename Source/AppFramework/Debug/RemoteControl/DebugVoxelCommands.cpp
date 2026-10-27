#include "AppFramework/Debug/RemoteControl/DebugVoxelCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/Debug/VoxelOccupancyViewer.h"
#include "AppFramework/Debug/VoxelStreamOverlay.h"
#include "AppFramework/Debug/WindEffectsOverlay.h"
#include "AppFramework/Debug/WindFlowOverlay.h"
#include "AppFramework/Debug/WindParticleGpuCheck.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/StaticModel.h"
#include "Renderer/Capture/PngWriter.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/IsoSurfaceRenderMesh.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Renderer/Voxel/BrickOccupancySelfTest.h"
#include "Renderer/Voxel/GpuSdfTextureReadback.h"
#include "Renderer/Voxel/GpuSolidVoxelizerSelfTest.h"
#include "Renderer/Voxel/GustedWindField.h"
#include "Renderer/Voxel/SceneVoxelization.h"
#include "Renderer/Voxel/SdfIsoSurface.h"
#include "Renderer/Voxel/SdfIsoSurfaceSelfTest.h"
#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/SimGridSolidMask.h"
#include "Renderer/Voxel/SimGridSolidMaskSelfTest.h"
#include "Renderer/Voxel/SolidVoxelizerSelfTest.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/VoxelCellSelfTest.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"
#include "Renderer/Voxel/VoxelSdfSelfTest.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/ProjectedWindSelfTest.h"
#include "Renderer/Voxel/WindEffectsSelfTest.h"
#include "Renderer/Voxel/WindFieldSelfTest.h"
#include "Renderer/Voxel/WindParticleImage.h"
#include "Renderer/Voxel/WindParticleSystem.h"
#include "Renderer/Voxel/SdfWindField.h"
#include "Renderer/Voxel/VoxelizerSelfTest.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // The last voxel.sdf upload. Nothing samples it yet, so replacing it frees the old
        // texture right away; the device releases a leftover one on shutdown.
        Render::RhiTextureHandle g_sdfTexture{};
        Render::IRHIDevice* g_sdfTextureDevice = nullptr;

        // The model `voxel.sdf.mesh show` added. Raw pointer, so it is only trusted while it is still
        // in the app's static model list (the editor or a scene load may have deleted it).
        StaticModel* g_sdfMeshModel = nullptr;

        StaticModel* FindSdfMeshModel(const ApplicationCore& app)
        {
            if (!g_sdfMeshModel) {
                return nullptr;
            }
            const std::vector<StaticModel*> models = app.GetStaticModels();
            if (std::find(models.begin(), models.end(), g_sdfMeshModel) == models.end()) {
                g_sdfMeshModel = nullptr;
            }
            return g_sdfMeshModel;
        }

        bool RemoveSdfMeshModel(ApplicationCore& app)
        {
            StaticModel* model = FindSdfMeshModel(app);
            if (!model) {
                return false;
            }
            app.DeleteObject(model);
            app.InvalidateRenderObjects();
            g_sdfMeshModel = nullptr;
            return true;
        }

        // Compute readback for `voxel.sdf check`, rebuilt when the device changes.
        std::unique_ptr<Render::Voxel::GpuSdfTextureReadback> g_sdfReadback;
        Render::IRHIDevice* g_sdfReadbackDevice = nullptr;
    }

    void RegisterVoxelCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("voxel.selftest", "run CPU vs GPU surface voxelizer parity test (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: voxel.selftest";
                }
                IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                if (!device) {
                    return "ERR no RHI device";
                }
                return Render::Voxel::RunVoxelizerSelfTest(*device);
            });

        registry.Register("voxel.solidtest", "run CPU solid (parity) voxelizer test against analytic boxes, then CPU vs GPU bit identity (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: voxel.solidtest";
                }
                const std::string cpuReport = Render::Voxel::RunSolidVoxelizerSelfTest();
                IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                const std::string gpuReport = device ? Render::Voxel::RunGpuSolidVoxelizerSelfTest(*device)
                                                     : std::string("ERR no RHI device");
                const bool ok = cpuReport.rfind("OK", 0) == 0 && gpuReport.rfind("OK", 0) == 0;
                return std::string(ok ? "OK" : "ERR") + " cpu: " + cpuReport + " | gpu: " + gpuReport;
            });

        registry.Register("voxel.scene", "[maxDim=128] [solid] [cpu] - voxelize opaque scene geometry on the GPU (1 bit/voxel) and report occupancy; solid also fills closed interiors by crossing parity and counts +X leak rows; cpu also runs the CPU references and diffs (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: voxel.scene [maxDim 8..512] [solid] [cpu]";
                    int maxDim = 128;
                    bool runCpu = false;
                    bool solid = false;
                    bool maxDimSet = false;
                    for (const std::string& arg : args) {
                        if (arg == "cpu" && !runCpu) {
                            runCpu = true;
                        } else if (arg == "solid" && !solid) {
                            solid = true;
                        } else if (!maxDimSet) {
                            const auto value = ParseInt(arg);
                            if (!value || *value < 8 || *value > 512) {
                                return USAGE;
                            }
                            maxDim = *value;
                            maxDimSet = true;
                        } else {
                            return USAGE;
                        }
                    }
                    const Render::Voxel::VoxelFillMode fillMode = solid ? Render::Voxel::VoxelFillMode::Solid
                                                                         : Render::Voxel::VoxelFillMode::Surface;

                    IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                    if (!device) {
                        return "ERR no RHI device";
                    }

                    Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                    std::string error;
                    if (!vox.Run(*device, app.GetRenderer().GetSceneSubmitter(), static_cast<uint32_t>(maxDim), fillMode, runCpu, error)) {
                        return "ERR " + error;
                    }

                    const Render::Voxel::VoxelGridDesc& desc = vox.Desc();
                    const Render::Voxel::SceneVoxelizationStats& stats = vox.Stats();
                    const Render::Voxel::OccupancyBitGrid& grid = vox.Grid();
                    const uint64_t voxelCount = grid.VoxelCount();
                    const double ratio = voxelCount > 0 ? static_cast<double>(stats.occupiedVoxels) / static_cast<double>(voxelCount) * 100.0 : 0.0;

                    std::ostringstream out;
                    out << std::fixed;
                    out << "OK items=" << stats.drawItemsUsed
                        << " skipped=" << stats.drawItemsSkipped
                        << " tris=" << stats.triangleCount
                        << " verts=" << stats.vertexCount
                        << " grid=" << desc.dimX << "x" << desc.dimY << "x" << desc.dimZ
                        << " voxel=" << std::setprecision(4) << desc.voxelSize
                        << " occupied=" << stats.occupiedVoxels
                        << " ratio=" << std::setprecision(3) << ratio << "%"
                        << " bytes=" << grid.SizeInBytes()
                        << std::setprecision(1)
                        << " gather=" << stats.gatherMs << "ms"
                        << " gpu=" << stats.gpuMs << "ms";
                    if (stats.cpuChecked) {
                        out << " cpu=" << stats.cpuMs << "ms diffBits=" << stats.diffBits;
                    }
                    if (stats.fillMode == Render::Voxel::VoxelFillMode::Solid) {
                        out << " surface=" << stats.surfaceVoxels
                            << " interior=" << stats.interiorVoxels
                            << " leakRows=" << stats.leakRows
                            << " solidGpu=" << stats.solidGpuMs << "ms";
                        if (stats.cpuChecked) {
                            out << " solidCpu=" << stats.solidCpuMs << "ms solidDiffBits=" << stats.solidDiffBits;
                        }
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("voxel.update", "<full|delta> [check] - bring the latest voxel.scene result up to date after objects moved; delta re-voxelizes only the moved regions (falls back to full when needed), check also runs a full GPU voxelization and diffs it (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                const char* const USAGE = "ERR usage: voxel.update <full|delta> [check]";
                try {
                    if (args.empty() || args.size() > 2 || (args[0] != "full" && args[0] != "delta")
                        || (args.size() == 2 && args[1] != "check")) {
                        return USAGE;
                    }
                    const Render::Voxel::VoxelUpdateMode mode = args[0] == "delta" ? Render::Voxel::VoxelUpdateMode::Delta
                                                                                    : Render::Voxel::VoxelUpdateMode::Full;
                    const bool check = args.size() == 2;

                    IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                    if (!device) {
                        return "ERR no RHI device";
                    }

                    Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                    if (!vox.HasResult()) {
                        return "ERR no voxel.scene result (run voxel.scene first)";
                    }
                    std::string error;
                    if (!vox.Update(*device, app.GetRenderer().GetSceneSubmitter(), mode, check, error)) {
                        return "ERR " + error;
                    }

                    const Render::Voxel::SceneVoxelizationStats& stats = vox.Stats();
                    std::ostringstream out;
                    out << std::fixed;
                    out << "OK mode=" << (stats.delta ? "delta" : "full")
                        << " fallback=" << (stats.fallbackReason.empty() ? "none" : stats.fallbackReason)
                        << " moved=" << stats.movedItems
                        << " regions=" << stats.regions
                        << " regionVoxels=" << stats.regionVoxels
                        << " occupied=" << stats.occupiedVoxels
                        << std::setprecision(1)
                        << " gatherMs=" << stats.gatherMs
                        << " gpuMs=" << stats.gpuMs
                        << " gen=" << vox.Generation();
                    if (stats.fullChecked) {
                        out << " fullCheckMs=" << stats.fullCheckMs << " fullDiffBits=" << stats.fullDiffBits;
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("voxel.cells", "<coverage|material|selftest> - build 4-bit cells for the latest voxel.scene grid; coverage = filled 2x2x2 sub-voxels (0..8) from a 2x resolution pass (coarse dims <= 256), material = material class per cell (1 generic, 2 metal, 3 emissive), selftest = CPU-only checks (build modes block until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                const char* const USAGE = "ERR usage: voxel.cells <coverage|material|selftest>";
                try {
                    if (args.size() != 1 || (args[0] != "coverage" && args[0] != "material" && args[0] != "selftest")) {
                        return USAGE;
                    }
                    if (args[0] == "selftest") {
                        return Render::Voxel::RunVoxelCellSelfTest();
                    }
                    const Render::Voxel::VoxelCellMode mode = args[0] == "material" ? Render::Voxel::VoxelCellMode::MaterialClass
                                                                                     : Render::Voxel::VoxelCellMode::Coverage;

                    IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                    if (!device) {
                        return "ERR no RHI device";
                    }

                    Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                    if (!vox.HasResult()) {
                        return "ERR no voxel.scene result (run voxel.scene first)";
                    }
                    std::string error;
                    if (!vox.RunCells(*device, app.GetRenderer().GetSceneSubmitter(), mode, error)) {
                        return "ERR " + error;
                    }

                    const Render::Voxel::VoxelGridDesc& desc = vox.Desc();
                    const Render::Voxel::VoxelCellStats& stats = vox.CellStats();
                    std::ostringstream out;
                    out << std::fixed;
                    out << "OK mode=" << (mode == Render::Voxel::VoxelCellMode::MaterialClass ? "material" : "coverage")
                        << " dims=" << desc.dimX << "x" << desc.dimY << "x" << desc.dimZ
                        << " nonZero=" << stats.nonZeroCells
                        << " bytes=" << stats.bytes
                        << " mismatch=";
                    if (stats.occupancyMismatches == UINT64_MAX) {
                        out << "n/a";
                    } else {
                        out << stats.occupancyMismatches;
                    }
                    out << " passes=" << stats.passes
                        << " items=" << stats.drawItemsUsed
                        << std::setprecision(1)
                        << " gatherMs=" << stats.gatherMs
                        << " gpuMs=" << stats.gpuMs
                        << " buildMs=" << stats.buildMs
                        << " hist=";
                    bool firstBin = true;
                    for (size_t v = 1; v < stats.histogram.size(); ++v) {
                        if (stats.histogram[v] == 0) {
                            continue;
                        }
                        out << (firstBin ? "" : ",") << v << ":" << stats.histogram[v];
                        firstBin = false;
                    }
                    out << " gen=" << vox.CellsGeneration();
                    if (mode == Render::Voxel::VoxelCellMode::MaterialClass) {
                        out << " classItems=generic:" << stats.itemsPerClass[1]
                            << ",metal:" << stats.itemsPerClass[2]
                            << ",emissive:" << stats.itemsPerClass[3];
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("voxel.sdf", "[selftest|upload|check] - build a signed distance field (exact EDT, Felzenszwalb & Huttenlocher 2012) from the latest voxel.scene grid and report it in voxel units; selftest = CPU brute-force checks; upload = also copy it into an R32F 3D texture through the RHI; check = upload, then read the texture back with a compute pass and count texels whose bits differ from the CPU field (DX12 only)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() > 1 || (args.size() == 1 && args[0] != "selftest" && args[0] != "upload" && args[0] != "check")) {
                    return "ERR usage: voxel.sdf [selftest|upload|check]";
                }
                const bool check = !args.empty() && args[0] == "check";
                const bool upload = check || (!args.empty() && args[0] == "upload");
                if (!args.empty() && !upload) {
                    return Render::Voxel::RunVoxelSdfSelfTest();
                }
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (!vox.HasResult()) {
                    return "ERR no voxel.scene result (run voxel.scene first)";
                }

                const auto start = std::chrono::steady_clock::now();
                Render::Voxel::SignedDistanceGrid field;
                Render::Voxel::BuildSignedDistanceField(vox.Grid(), field);
                const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

                // Range over voxels that have a feature on their side (the rest read +-NoFeatureDistance).
                const float noFeature = field.NoFeatureDistance();
                float minValue = 0.0f;
                float maxValue = 0.0f;
                uint64_t negative = 0;
                uint64_t noFeatureCount = 0;
                for (float value : field.Values()) {
                    if (value == noFeature || value == -noFeature) {
                        ++noFeatureCount;
                        continue;
                    }
                    negative += value < 0.0f ? 1 : 0;
                    minValue = value < minValue ? value : minValue;
                    maxValue = value > maxValue ? value : maxValue;
                }

                const Render::Voxel::VoxelGridDesc& desc = vox.Desc();
                std::ostringstream out;
                out << std::fixed << std::setprecision(2);
                out << "OK dims=" << desc.dimX << "x" << desc.dimY << "x" << desc.dimZ
                    << " voxelSize=" << std::setprecision(4) << desc.voxelSize << std::setprecision(2)
                    << " occupied=" << vox.Grid().CountOccupied()
                    << " negative=" << negative
                    << " signMismatch=" << field.CountSignMismatches(vox.Grid())
                    << " noFeature=" << noFeatureCount
                    << " min=" << minValue << " max=" << maxValue
                    << " bytes=" << field.SizeInBytes()
                    << std::setprecision(1) << " buildMs=" << buildMs
                    << " gen=" << vox.Generation();

                if (upload) {
                    IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                    if (!device) {
                        return "ERR no RHI device";
                    }
                    if (g_sdfTexture.IsValid() && g_sdfTextureDevice == device) {
                        device->DestroyRhiResource(g_sdfTexture);
                    }
                    g_sdfTexture = {};
                    g_sdfTextureDevice = nullptr;

                    const auto uploadStart = std::chrono::steady_clock::now();
                    const uint32_t rowPitch = desc.dimX * static_cast<uint32_t>(sizeof(float));
                    const Render::RhiTextureHandle texture = device->CreateRhiTexture3DFromData(
                        desc.dimX, desc.dimY, desc.dimZ, field.Values().data(), rowPitch, rowPitch * desc.dimY,
                        Render::RhiFormat::R32Float);
                    const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
                    if (!texture.IsValid()) {
                        return "ERR 3D texture upload failed (" + out.str() + ")";
                    }
                    g_sdfTexture = texture;
                    g_sdfTextureDevice = device;
                    out << " uploaded=1 uploadMs=" << uploadMs;

                    if (check) {
                        if (!g_sdfReadback || g_sdfReadbackDevice != device) {
                            g_sdfReadback = std::make_unique<Render::Voxel::GpuSdfTextureReadback>();
                            g_sdfReadbackDevice = device;
                        }
                        std::string error;
                        if (!g_sdfReadback->Initialize(*device, error)) {
                            return "ERR " + error + " (" + out.str() + ")";
                        }
                        const auto readStart = std::chrono::steady_clock::now();
                        std::vector<float> texels;
                        if (!g_sdfReadback->Readback(texture, desc.dimX, desc.dimY, desc.dimZ, texels, error)) {
                            return "ERR " + error + " (" + out.str() + ")";
                        }
                        const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count();
                        // Bit comparison: R32F Load must return exactly what was uploaded.
                        uint64_t mismatch = 0;
                        const std::vector<float>& values = field.Values();
                        for (size_t i = 0; i < values.size(); ++i) {
                            mismatch += std::memcmp(&values[i], &texels[i], sizeof(float)) != 0 ? 1 : 0;
                        }
                        out << " readback=" << texels.size() << " mismatch=" << mismatch << " readMs=" << readMs;
                    }
                }
                return out.str();
            });

        registry.Register("voxel.sdf.mesh", "[selftest | show [r g b] | hide] - mesh the zero level set of the latest voxel.scene SDF with Marching Tetrahedra (the particle iso-surface extractor) and report triangles, open edges, enclosed volume against the occupied voxels, and timings; selftest = CPU checks on synthetic grids; show = also add the mesh to the scene as an opaque static model (replacing the previous one), hide = remove it",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.sdf.mesh [selftest | show [r g b] | hide]";
                const std::string verb = args.empty() ? std::string() : args[0];
                if (verb == "selftest" && args.size() == 1) {
                    return Render::Voxel::RunSdfIsoSurfaceSelfTest();
                }
                if (verb == "hide" && args.size() == 1) {
                    return RemoveSdfMeshModel(app) ? "OK hidden" : "OK nothing shown";
                }
                const bool show = verb == "show";
                if (!(args.empty() || (show && (args.size() == 1 || args.size() == 4)))) {
                    return USAGE;
                }
                // Light grey-blue by eye, so the voxel shell reads apart from the textured scene.
                float tint[3] = { 0.72f, 0.78f, 0.86f };
                if (show && args.size() == 4) {
                    for (int c = 0; c < 3; ++c) {
                        const auto value = ParseFloat(args[size_t(c) + 1]);
                        if (!value || *value < 0.0f || *value > 1.0f) {
                            return USAGE;
                        }
                        tint[c] = *value;
                    }
                }
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (!vox.HasResult()) {
                    return "ERR no voxel.scene result (run voxel.scene first)";
                }

                const auto start = std::chrono::steady_clock::now();
                Render::Voxel::SignedDistanceGrid field;
                Render::Voxel::BuildSignedDistanceField(vox.Grid(), field);
                const auto sdfDone = std::chrono::steady_clock::now();
                Render::Fluid::IsoSurfaceMesh mesh;
                Render::Voxel::ExtractSdfIsoSurface(field, vox.Desc(), mesh);
                const auto meshDone = std::chrono::steady_clock::now();
                const double sdfMs = std::chrono::duration<double, std::milli>(sdfDone - start).count();
                const double meshMs = std::chrono::duration<double, std::milli>(meshDone - sdfDone).count();

                const Render::Voxel::VoxelGridDesc& desc = vox.Desc();
                const double voxelVolume = double(vox.Grid().CountOccupied())
                                         * desc.voxelSize * desc.voxelSize * desc.voxelSize;
                const double volume = Render::Fluid::IsoSurfaceVolume(mesh);
                std::ostringstream out;
                out << std::fixed << std::setprecision(2);
                out << "OK dims=" << desc.dimX << "x" << desc.dimY << "x" << desc.dimZ
                    << " occupied=" << vox.Grid().CountOccupied()
                    << " tris=" << mesh.TriangleCount()
                    << " verts=" << mesh.positions.size() / 3
                    << " openEdges=" << Render::Fluid::CountOpenEdges(mesh)
                    << " volume=" << volume
                    << " voxelVolume=" << voxelVolume
                    << " area=" << Render::Fluid::IsoSurfaceArea(mesh)
                    << " sdfMs=" << sdfMs << " meshMs=" << meshMs;
                if (show) {
                    if (mesh.TriangleCount() == 0) {
                        return "ERR empty mesh, nothing to show";
                    }
                    const auto convertStart = std::chrono::steady_clock::now();
                    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    Render::Mesh renderMesh = Render::Fluid::BuildIsoSurfaceRenderMesh(mesh, white);
                    RemoveSdfMeshModel(app);
                    StaticModel* model = app.CreateStaticModel();
                    if (!model) {
                        return "ERR could not create a static model";
                    }
                    model->AddStaticMesh(std::move(renderMesh),
                                         Render::MakeOpaqueSurfaceMaterial(tint[0], tint[1], tint[2], 0.8f, 0.0f));
                    app.InvalidateRenderObjects();
                    g_sdfMeshModel = model;
                    const double convertMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - convertStart).count();
                    out << " shown=1 convertMs=" << convertMs;
                }
                return out.str();
            });

        registry.Register("voxel.sdf.trace", "<path.png> [width height] - sphere-trace (Hart 1996) the SDF of the latest voxel.scene grid from the main camera on the CPU and write a Lambert-shaded PNG; reports hits and step counts",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.sdf.trace <path.png> [width height]";
                if (args.size() != 1 && args.size() != 3) {
                    return USAGE;
                }
                uint32_t width = 480;
                uint32_t height = 270;
                if (args.size() == 3) {
                    const auto w = ParseInt(args[1]);
                    const auto h = ParseInt(args[2]);
                    if (!w || !h || *w <= 0 || *h <= 0 || *w > 1920 || *h > 1080) {
                        return USAGE;
                    }
                    width = static_cast<uint32_t>(*w);
                    height = static_cast<uint32_t>(*h);
                }
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (!vox.HasResult()) {
                    return "ERR no voxel.scene result (run voxel.scene first)";
                }
                Camera* camera = app.GetMainCamera();
                if (!camera) {
                    return "ERR no active camera";
                }
                const auto start = std::chrono::steady_clock::now();
                Render::Voxel::SignedDistanceGrid field;
                Render::Voxel::BuildSignedDistanceField(vox.Grid(), field);
                const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

                const Render::RenderCameraProxy proxy =
                    camera->BuildRenderCameraProxy(static_cast<float>(width), static_cast<float>(height));
                Render::Voxel::SdfTraceCamera traceCamera;
                for (int i = 0; i < 3; ++i) {
                    traceCamera.position[i] = proxy.cameraPosition[i];
                    traceCamera.right[i] = proxy.cameraRight[i];
                    traceCamera.up[i] = proxy.cameraUp[i];
                    traceCamera.forward[i] = proxy.cameraForward[i];
                }
                traceCamera.tanHalfFovY = proxy.tanHalfFovY;
                traceCamera.aspectRatio = proxy.aspectRatio;

                const auto traceStart = std::chrono::steady_clock::now();
                std::vector<uint8_t> pixels;
                Render::Voxel::SdfTraceImageStats stats;
                Render::Voxel::RenderSdfTraceImage(field, vox.Desc(), traceCamera, width, height,
                                                   Render::Voxel::SdfTraceSettings{}, pixels, stats);
                const double traceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - traceStart).count();
                if (!Render::SavePngBgra8(std::filesystem::path(args[0]).wstring(), pixels.data(), width, height)) {
                    return "ERR could not write " + args[0];
                }
                const uint64_t pixelCount = static_cast<uint64_t>(width) * height;
                std::ostringstream out;
                out << "OK " << width << "x" << height << " hits=" << stats.hitCount << "/" << pixelCount
                    << " avgSteps=" << (static_cast<double>(stats.totalSteps) / static_cast<double>(pixelCount))
                    << " maxSteps=" << stats.maxSteps << " buildMs=" << buildMs << " traceMs=" << traceMs;
                return out.str();
            });

        registry.Register("voxel.wind", "selftest | run <path.png> [count=N] [steps=N] [wind=x,y,z] [turb=T] [vps=V] [mode=curl|project] [warm=0|1] [slabs=N] - curl-noise wind (Bridson et al. 2007) or pressure-projected wind (mode=project; turb defaults to 0 there, turb>0 adds curl-noise gusts; warm=1 starts the pressure solve from the previous project run on a same-sized grid) bounded by the SDF of the latest voxel.scene grid; run advects tracer particles on the CPU and draws their streaks over a sphere-traced view from the main camera",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.wind selftest | run <path.png> [count=N] [steps=N] [wind=x,y,z] [turb=T] [vps=V] [mode=curl|project] [warm=0|1] [slabs=N]";
                if (args.size() == 1 && args[0] == "selftest") {
                    const std::string curl = Render::Voxel::RunWindFieldSelfTest();
                    const std::string project = Render::Voxel::RunProjectedWindSelfTest();
                    const bool failed = curl.rfind("ERR", 0) == 0 || project.rfind("ERR", 0) == 0;
                    return std::string(failed ? "ERR " : "") + "curl: " + curl + " | project: " + project;
                }
                if (args.size() < 2 || args[0] != "run") {
                    return USAGE;
                }
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (!vox.HasResult()) {
                    return "ERR no voxel.scene result (run voxel.scene first)";
                }
                Camera* camera = app.GetMainCamera();
                if (!camera) {
                    return "ERR no active camera";
                }
                const Render::Voxel::VoxelGridDesc& desc = vox.Desc();
                const float extent = desc.voxelSize * static_cast<float>((std::max)({ desc.dimX, desc.dimY, desc.dimZ }));

                // Defaults scale with the grid: the wind crosses the largest extent in 10 s.
                uint32_t count = 20000;
                uint32_t steps = 120;
                Render::Voxel::SdfWindSettings settings;
                settings.wind[0] = extent / 10.0f;
                settings.wind[1] = 0.0f;
                settings.wind[2] = 0.0f;
                settings.turbulence = 0.3f;
                float voxelsPerStep = 2.0f;
                bool projectMode = false;
                bool turbulenceGiven = false;
                bool warmStart = false;
                uint32_t micSlabs = 1;
                for (size_t i = 2; i < args.size(); ++i) {
                    const size_t eq = args[i].find('=');
                    if (eq == std::string::npos) {
                        return USAGE;
                    }
                    const std::string key = args[i].substr(0, eq);
                    const std::string value = args[i].substr(eq + 1);
                    if (key == "count" || key == "steps") {
                        const auto n = ParseInt(value);
                        if (!n || *n <= 0 || *n > 200000) {
                            return USAGE;
                        }
                        (key == "count" ? count : steps) = static_cast<uint32_t>(*n);
                    } else if (key == "mode") {
                        if (value == "project") {
                            projectMode = true;
                        } else if (value == "curl") {
                            projectMode = false;
                        } else {
                            return USAGE;
                        }
                    } else if (key == "warm") {
                        if (value != "0" && value != "1") {
                            return USAGE;
                        }
                        warmStart = value == "1";
                    } else if (key == "slabs") {
                        const auto n = ParseInt(value);
                        if (!n || *n <= 0 || *n > 1024) {
                            return USAGE;
                        }
                        micSlabs = static_cast<uint32_t>(*n);
                    } else if (key == "turb") {
                        const auto t = ParseFloat(value);
                        if (!t || *t < 0.0f) {
                            return USAGE;
                        }
                        settings.turbulence = *t;
                        turbulenceGiven = true;
                    } else if (key == "vps") {
                        const auto v = ParseFloat(value);
                        if (!v || *v <= 0.0f || *v > 16.0f) {
                            return USAGE;
                        }
                        voxelsPerStep = *v;
                    } else if (key == "wind") {
                        const size_t c0 = value.find(',');
                        const size_t c1 = c0 == std::string::npos ? c0 : value.find(',', c0 + 1);
                        if (c1 == std::string::npos) {
                            return USAGE;
                        }
                        const auto x = ParseFloat(value.substr(0, c0));
                        const auto y = ParseFloat(value.substr(c0 + 1, c1 - c0 - 1));
                        const auto z = ParseFloat(value.substr(c1 + 1));
                        if (!x || !y || !z) {
                            return USAGE;
                        }
                        settings.wind[0] = *x;
                        settings.wind[1] = *y;
                        settings.wind[2] = *z;
                    } else {
                        return USAGE;
                    }
                }
                const float windSpeed = std::sqrt(settings.wind[0] * settings.wind[0] + settings.wind[1] * settings.wind[1]
                                                  + settings.wind[2] * settings.wind[2]);
                if (windSpeed <= 0.0f || static_cast<uint64_t>(count) * steps > 6000000ull) {
                    return "ERR wind must be nonzero and count*steps <= 6e6 (the debug pipe times out after 5 s)";
                }
                settings.rampWidth = 4.0f * desc.voxelSize;
                settings.noiseScale = 16.0f * desc.voxelSize;
                // project is steady unless turb= is given; gusts cost about as much as a curl run, and
                // project + gusts at the defaults would not fit the 5 s command timeout.
                if (projectMode && !turbulenceGiven) {
                    settings.turbulence = 0.0f;
                }
                // Two voxels per step by default, so the 8-entry trail spans about 16 voxels.
                const float dt = voxelsPerStep * desc.voxelSize / windSpeed;

                const auto start = std::chrono::steady_clock::now();
                Render::Voxel::SignedDistanceGrid field;
                Render::Voxel::BuildSignedDistanceField(vox.Grid(), field);
                const auto simStart = std::chrono::steady_clock::now();
                Render::Voxel::ProjectedWindSettings projectedSettings;
                for (int i = 0; i < 3; ++i) {
                    projectedSettings.wind[i] = settings.wind[i];
                }
                projectedSettings.projection.micSlabs = micSlabs;
                Render::Voxel::SdfWindField curlWind(field, desc, settings);
                std::unique_ptr<Render::Voxel::ProjectedWindField> projectedWind;
                // Pressure of the last project run; any same-sized guess is valid, a close one just converges sooner.
                static std::vector<float> s_lastPressure;
                if (projectMode) {
                    const bool useWarm = warmStart && s_lastPressure.size() == static_cast<size_t>(desc.dimX) * desc.dimY * desc.dimZ;
                    projectedWind = std::make_unique<Render::Voxel::ProjectedWindField>(vox.Grid(), field, desc, projectedSettings,
                                                                                         useWarm ? &s_lastPressure : nullptr);
                    s_lastPressure = projectedWind->Pressure();
                }
                // Gusts: curl noise without its laminar term, added to the projected base (6a-4a).
                Render::Voxel::SdfWindSettings gustSettings = settings;
                gustSettings.laminar = false;
                Render::Voxel::SdfWindField gustWind(field, desc, gustSettings);
                std::unique_ptr<Render::Voxel::GustedWindField> gustedWind;
                const bool gusts = projectMode && settings.turbulence > 0.0f;
                if (gusts) {
                    gustedWind = std::make_unique<Render::Voxel::GustedWindField>(*projectedWind, gustWind);
                }
                const double solveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - simStart).count();
                const Render::Voxel::IWindVelocityField& wind = gusts
                    ? static_cast<const Render::Voxel::IWindVelocityField&>(*gustedWind)
                    : projectMode ? static_cast<const Render::Voxel::IWindVelocityField&>(*projectedWind)
                                  : static_cast<const Render::Voxel::IWindVelocityField&>(curlWind);
                Render::Voxel::WindParticleSystem particles;
                particles.Reset(wind, count, 1u, 40.0f * dt, 120.0f * dt, desc.voxelSize);
                uint64_t outside = 0;
                uint64_t solid = 0;
                uint64_t aged = 0;
                Render::Voxel::WindParticleSystem::StepStats last;
                for (uint32_t i = 0; i < steps; ++i) {
                    const float time = static_cast<float>(i) * dt;
                    if (gusts) {
                        gustWind.SetTime(time);
                    } else if (!projectMode) {
                        curlWind.SetTime(time);
                    }
                    last = particles.Step(wind, dt);
                    outside += last.respawnedOutside;
                    solid += last.respawnedSolid;
                    aged += last.respawnedAge;
                }
                const auto simEnd = std::chrono::steady_clock::now();

                constexpr uint32_t WIDTH = 480;
                constexpr uint32_t HEIGHT = 270;
                const Render::RenderCameraProxy proxy =
                    camera->BuildRenderCameraProxy(static_cast<float>(WIDTH), static_cast<float>(HEIGHT));
                Render::Voxel::SdfTraceCamera traceCamera;
                for (int i = 0; i < 3; ++i) {
                    traceCamera.position[i] = proxy.cameraPosition[i];
                    traceCamera.right[i] = proxy.cameraRight[i];
                    traceCamera.up[i] = proxy.cameraUp[i];
                    traceCamera.forward[i] = proxy.cameraForward[i];
                }
                traceCamera.tanHalfFovY = proxy.tanHalfFovY;
                traceCamera.aspectRatio = proxy.aspectRatio;
                std::vector<uint8_t> pixels;
                std::vector<float> depth;
                Render::Voxel::SdfTraceImageStats traceStats;
                Render::Voxel::RenderSdfTraceImage(field, desc, traceCamera, WIDTH, HEIGHT, Render::Voxel::SdfTraceSettings{},
                                                   pixels, traceStats, &depth);
                const uint64_t drawn = Render::Voxel::DrawWindParticleTrails(particles, traceCamera, WIDTH, HEIGHT, depth,
                                                                             windSpeed, pixels);
                const auto end = std::chrono::steady_clock::now();
                if (!Render::SavePngBgra8(std::filesystem::path(args[1]).wstring(), pixels.data(), WIDTH, HEIGHT)) {
                    return "ERR could not write " + args[1];
                }
                using Ms = std::chrono::duration<double, std::milli>;
                const uint64_t moves = static_cast<uint64_t>(count) * steps;
                std::ostringstream out;
                out << "OK count=" << count << " steps=" << steps << " vps=" << voxelsPerStep << " dt=" << dt << " wind=" << settings.wind[0] << ","
                    << settings.wind[1] << "," << settings.wind[2] << " turb=" << settings.turbulence
                    << " penetrations=" << solid << " (" << (100.0 * static_cast<double>(solid) / static_cast<double>(moves))
                    << "%) outside=" << outside << " aged=" << aged << " meanSpeed=" << last.meanSpeed
                    << " maxSpeed=" << last.maxSpeed << " drawnPixels=" << drawn
                    << " sdfMs=" << Ms(simStart - start).count() << " simMs=" << Ms(simEnd - simStart).count()
                    << " drawMs=" << Ms(end - simEnd).count();
                if (projectMode) {
                    const Render::Voxel::PressureProjectionStats& ps = projectedWind->Stats();
                    out << " mode=project gusts=" << (gusts ? 1 : 0) << " iters=" << ps.iterations << " converged=" << (ps.converged ? 1 : 0)
                        << " warm=" << (ps.warmStarted ? 1 : 0) << " slabs=" << ps.micSlabs
                        << " enclosed=" << ps.enclosedCells << " fluid=" << ps.fluidCells
                        << " finalDiv=" << ps.finalMaxDivergence << " solveMs=" << solveMs
                        << " pcgMs=" << ps.solveMs << " preconMs=" << ps.preconditionerMs;
                } else {
                    out << " mode=curl";
                }
                return out.str();
            });

        registry.Register("voxel.wind.overlay", "on [count=N] [mode=project|curl] [wind=x,y,z] [turb=T] [speed=S] [slabs=N] [occlude=0|1] [dash=0|1] [tracers=0|1] [source=scene|stream] | off | status - draws wind tracer trails over the normal scene every frame (ImGui background lines, main camera); the field is built once from the latest voxel.scene grid (source=stream: from the voxel.stream window around the camera, re-projected in the background when the camera chunk changes; project mode, needs voxel.stream on sdf=A), occlusion is an SDF ray per particle",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.wind.overlay on [count=N] [mode=project|curl] [wind=x,y,z] [turb=T] [speed=S] [slabs=N] [occlude=0|1] [dash=0|1] [tracers=0|1] [source=scene|stream] | off | status";
                Debug::WindFlowOverlay& overlay = app.GetWindFlowOverlay();
                if (args.size() == 1 && args[0] == "off") {
                    overlay.Disable();
                    return "OK overlay=0";
                }
                if (args.size() == 1 && args[0] == "status") {
                    return overlay.Status();
                }
                if (args.empty() || args[0] != "on") {
                    return USAGE;
                }
                Debug::WindFlowOverlaySettings settings;
                for (size_t i = 1; i < args.size(); ++i) {
                    const size_t eq = args[i].find('=');
                    if (eq == std::string::npos) {
                        return USAGE;
                    }
                    const std::string key = args[i].substr(0, eq);
                    const std::string value = args[i].substr(eq + 1);
                    if (key == "count") {
                        const auto n = ParseInt(value);
                        if (!n || *n <= 0 || *n > 20000) {
                            return USAGE;
                        }
                        settings.count = static_cast<uint32_t>(*n);
                    } else if (key == "source") {
                        if (value != "scene" && value != "stream") {
                            return USAGE;
                        }
                        settings.streamWindow = value == "stream";
                    } else if (key == "mode") {
                        if (value != "project" && value != "curl") {
                            return USAGE;
                        }
                        settings.projectMode = value == "project";
                    } else if (key == "slabs") {
                        const auto n = ParseInt(value);
                        if (!n || *n <= 0 || *n > 1024) {
                            return USAGE;
                        }
                        settings.micSlabs = static_cast<uint32_t>(*n);
                    } else if (key == "turb" || key == "speed") {
                        const auto t = ParseFloat(value);
                        if (!t || *t < 0.0f) {
                            return USAGE;
                        }
                        (key == "turb" ? settings.turbulence : settings.timeScale) = *t;
                    } else if (key == "occlude" || key == "dash" || key == "tracers") {
                        if (value != "0" && value != "1") {
                            return USAGE;
                        }
                        (key == "occlude" ? settings.occlude : key == "dash" ? settings.dashed : settings.tracers) = value == "1";
                    } else if (key == "wind") {
                        const size_t c0 = value.find(',');
                        const size_t c1 = c0 == std::string::npos ? c0 : value.find(',', c0 + 1);
                        if (c1 == std::string::npos) {
                            return USAGE;
                        }
                        const auto x = ParseFloat(value.substr(0, c0));
                        const auto y = ParseFloat(value.substr(c0 + 1, c1 - c0 - 1));
                        const auto z = ParseFloat(value.substr(c1 + 1));
                        if (!x || !y || !z) {
                            return USAGE;
                        }
                        settings.wind[0] = *x;
                        settings.wind[1] = *y;
                        settings.wind[2] = *z;
                    } else {
                        return USAGE;
                    }
                }
                return overlay.Enable(app, settings);
            });

        registry.Register("voxel.stream", "on [chunk=N] [voxel=V] [radius=R] [jobs=J] [sdf=A] [boxes=0|1] | off | status | selftest | window [wind=x,y,z] [slabs=N] - camera-centred chunk streaming of surface voxels: chunks within radius chunks of the main camera are voxelized on the job system, chunks beyond radius+1 are unloaded; sdf=A (1..chunk) also builds each chunk's SDF clamped to +-(A-0.5) voxels from an A-voxel apron; draws the chunk boxes (green = loaded, yellow = loading). window (needs sdf=A) copies the loaded chunks within radius of the camera chunk into one grid and runs the wind projection on it synchronously (default wind 10,0,0 m/s, slabs 8)",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.stream on [chunk=N] [voxel=V] [radius=R] [jobs=J] [sdf=A] [boxes=0|1] | off | status | selftest | window [wind=x,y,z] [slabs=N]";
                Debug::VoxelStreamOverlay& overlay = app.GetVoxelStreamOverlay();
                if (!args.empty() && args[0] == "window") {
                    float wind[3] = { 10.0f, 0.0f, 0.0f };
                    uint32_t slabs = 8;
                    for (size_t i = 1; i < args.size(); ++i) {
                        const size_t eq = args[i].find('=');
                        if (eq == std::string::npos) {
                            return USAGE;
                        }
                        const std::string key = args[i].substr(0, eq);
                        const std::string value = args[i].substr(eq + 1);
                        if (key == "wind") {
                            const size_t c0 = value.find(',');
                            const size_t c1 = c0 == std::string::npos ? c0 : value.find(',', c0 + 1);
                            if (c1 == std::string::npos) {
                                return USAGE;
                            }
                            const auto x = ParseFloat(value.substr(0, c0));
                            const auto y = ParseFloat(value.substr(c0 + 1, c1 - c0 - 1));
                            const auto z = ParseFloat(value.substr(c1 + 1));
                            if (!x || !y || !z) {
                                return USAGE;
                            }
                            wind[0] = *x;
                            wind[1] = *y;
                            wind[2] = *z;
                        } else if (key == "slabs") {
                            const auto n = ParseInt(value);
                            if (!n || *n < 1 || *n > 256) {
                                return USAGE;
                            }
                            slabs = static_cast<uint32_t>(*n);
                        } else {
                            return USAGE;
                        }
                    }
                    return overlay.ProjectWindow(app, wind, slabs);
                }
                if (args.size() == 1 && args[0] == "off") {
                    return overlay.Stop();
                }
                if (args.size() == 1 && args[0] == "status") {
                    return overlay.Status();
                }
                if (args.size() == 1 && args[0] == "selftest") {
                    std::string report;
                    Render::Voxel::VoxelChunkStreamer::SelfTest(report);
                    return report;
                }
                if (args.empty() || args[0] != "on") {
                    return USAGE;
                }
                Render::Voxel::VoxelChunkStreamerSettings settings;
                bool boxes = true;
                for (size_t i = 1; i < args.size(); ++i) {
                    const size_t eq = args[i].find('=');
                    if (eq == std::string::npos) {
                        return USAGE;
                    }
                    const std::string key = args[i].substr(0, eq);
                    const std::string value = args[i].substr(eq + 1);
                    if (key == "chunk" || key == "radius" || key == "jobs" || key == "sdf") {
                        const auto n = ParseInt(value);
                        if (!n) {
                            return USAGE;
                        }
                        if (key == "chunk") {
                            if (*n < 4 || *n > 256) {
                                return USAGE;
                            }
                            settings.chunkVoxels = static_cast<uint32_t>(*n);
                        } else if (key == "radius") {
                            if (*n < 0 || *n > 16) {
                                return USAGE;
                            }
                            settings.loadRadius = static_cast<uint32_t>(*n);
                        } else if (key == "sdf") {
                            if (*n < 0 || *n > 256) {
                                return USAGE;
                            }
                            settings.sdfApronVoxels = static_cast<uint32_t>(*n);
                        } else {
                            if (*n < 1 || *n > 8) {
                                return USAGE;
                            }
                            settings.maxJobsInFlight = static_cast<uint32_t>(*n);
                        }
                    } else if (key == "voxel") {
                        const auto v = ParseFloat(value);
                        if (!v || !(*v > 0.0f)) {
                            return USAGE;
                        }
                        settings.voxelSize = *v;
                    } else if (key == "boxes") {
                        if (value != "0" && value != "1") {
                            return USAGE;
                        }
                        boxes = value == "1";
                    } else {
                        return USAGE;
                    }
                }
                // The apron may reach at most one neighbouring chunk on each side.
                if (settings.sdfApronVoxels > settings.chunkVoxels) {
                    return USAGE;
                }
                overlay.SetDrawBoxes(boxes);
                return overlay.Start(app, settings);
            });

        registry.Register("voxel.wind.fx","smoke|fire|petals on|add [at=x,y,z | screen=x,y] [count=N] [rate=R] [radius=V] | <effect> undo | <effect> off | off | status | selftest | tracers 0|1 | surface 0|1|mesh | volume 0|1 | volume style [smoke=S] [flame=F] [emit=E] [soot=K] | gpu 0|1 | gpucheck [smoke|petals] [count=N] [steps=N] - smoke, spreading fire and petal showers carried by the wind overlay's field (needs voxel.wind.overlay on). on restarts the effect with one source, add appends a source (rate is per source); screen= picks the surface under a viewport pixel, the default is where the camera looks. fire radius= caps the spread distance from the sources in voxels (default 12, 0 = unlimited). surface mesh draws the flame/smoke iso-surfaces as renderer meshes (works with either render.meshshader setting; the surfaces stay on the vertex path). status reports per source the mean drift of its particles and its cosine to the local wind",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.wind.fx smoke|fire|petals on|add [at=x,y,z | screen=x,y] [count=N] [rate=R] [radius=V] | <effect> undo | <effect> off | off | status | selftest | tracers 0|1 | surface 0|1|mesh | gpu 0|1 | gpucheck [smoke|petals] [count=N] [steps=N]";
                Debug::WindEffectsOverlay& fx = app.GetWindEffectsOverlay();
                if (!args.empty() && args[0] == "gpucheck") {
                    bool petals = false;
                    uint32_t count = 4000;
                    uint32_t steps = 120;
                    for (size_t i = 1; i < args.size(); ++i) {
                        if (i == 1 && (args[i] == "smoke" || args[i] == "petals")) {
                            petals = args[i] == "petals";
                            continue;
                        }
                        const size_t eq = args[i].find('=');
                        if (eq == std::string::npos) {
                            return USAGE;
                        }
                        const std::string key = args[i].substr(0, eq);
                        const auto n = ParseInt(args[i].substr(eq + 1));
                        if (!n || *n <= 0 || (key != "count" && key != "steps")) {
                            return USAGE;
                        }
                        (key == "count" ? count : steps) = static_cast<uint32_t>(*n);
                    }
                    return Debug::RunWindParticleGpuCheck(app, petals, count, steps);
                }
                if (args.size() == 1 && args[0] == "selftest") {
                    return Render::Voxel::RunWindEffectsSelfTest();
                }
                if (args.size() == 1 && args[0] == "status") {
                    return fx.Status(app);
                }
                if (args.size() == 1 && args[0] == "off") {
                    fx.StopAll();
                    return "OK fx=off";
                }
                if (args.size() == 2 && args[0] == "tracers") {
                    if (args[1] != "0" && args[1] != "1") {
                        return USAGE;
                    }
                    app.GetWindFlowOverlay().SetTracersVisible(args[1] == "1");
                    return std::string("OK tracers=") + args[1];
                }
                if (args.size() == 2 && args[0] == "surface") {
                    if (args[1] == "mesh") {
                        return fx.SetSurfaceScene(app, true);
                    }
                    if (args[1] != "0" && args[1] != "1") {
                        return USAGE;
                    }
                    if (fx.IsSurfaceScene()) {
                        fx.SetSurfaceScene(app, false);
                    }
                    fx.SetSurface(args[1] == "1");
                    return std::string("OK surface=") + args[1];
                }
                if (args.size() >= 2 && args[0] == "volume" && args[1] == "style") {
                    Render::ParticleVolumeStyle style = app.GetRenderer().GetParticleVolumeStyle();
                    for (size_t i = 2; i < args.size(); ++i) {
                        const size_t eq = args[i].find('=');
                        if (eq == std::string::npos) {
                            return USAGE;
                        }
                        const std::string key = args[i].substr(0, eq);
                        const auto v = ParseFloat(args[i].substr(eq + 1));
                        if (!v || *v < 0.0f) {
                            return USAGE;
                        }
                        if (key == "smoke") {
                            style.smokeExtinction = *v;
                        } else if (key == "flame") {
                            style.flameExtinction = *v;
                        } else if (key == "emit") {
                            style.flameEmission = *v;
                        } else if (key == "soot") {
                            style.sootDarkening = *v;
                        } else {
                            return USAGE;
                        }
                    }
                    app.GetRenderer().SetParticleVolumeStyle(style);
                    std::ostringstream out;
                    out << "OK smoke=" << style.smokeExtinction << " flame=" << style.flameExtinction
                        << " emit=" << style.flameEmission << " soot=" << style.sootDarkening;
                    return out.str();
                }
                if (args.size() == 2 && args[0] == "volume") {
                    if (args[1] != "0" && args[1] != "1") {
                        return USAGE;
                    }
                    fx.SetVolume(app, args[1] == "1");
                    return std::string("OK volume=") + args[1];
                }
                if (args.size() == 2 && args[0] == "gpu") {
                    if (args[1] != "0" && args[1] != "1") {
                        return USAGE;
                    }
                    return fx.SetGpu(app, args[1] == "1");
                }
                if (args.size() < 2) {
                    return USAGE;
                }
                Debug::WindEffect effect;
                if (args[0] == "smoke") {
                    effect = Debug::WindEffect::Smoke;
                } else if (args[0] == "fire") {
                    effect = Debug::WindEffect::Fire;
                } else if (args[0] == "petals") {
                    effect = Debug::WindEffect::Petals;
                } else {
                    return USAGE;
                }
                if (args[1] == "off" && args.size() == 2) {
                    fx.Stop(effect);
                    return "OK fx=" + args[0] + " off";
                }
                if (args[1] == "undo" && args.size() == 2) {
                    return fx.UndoLast(effect);
                }
                if (args[1] != "on" && args[1] != "add") {
                    return USAGE;
                }
                const bool add = args[1] == "add";
                Debug::WindEffectSettings settings;
                for (size_t i = 2; i < args.size(); ++i) {
                    const size_t eq = args[i].find('=');
                    if (eq == std::string::npos) {
                        return USAGE;
                    }
                    const std::string key = args[i].substr(0, eq);
                    const std::string value = args[i].substr(eq + 1);
                    if (key == "count") {
                        const auto n = ParseInt(value);
                        if (!n || *n <= 0 || *n > 50000) {
                            return USAGE;
                        }
                        settings.count = static_cast<uint32_t>(*n);
                    } else if (key == "rate") {
                        const auto r = ParseFloat(value);
                        if (!r || *r < 0.0f) {
                            return USAGE;
                        }
                        settings.rate = *r;
                    } else if (key == "radius") {
                        const auto r = ParseFloat(value);
                        if (!r || *r < 0.0f) {
                            return USAGE;
                        }
                        settings.radius = *r;
                    } else if (key == "at") {
                        const size_t c0 = value.find(',');
                        const size_t c1 = c0 == std::string::npos ? c0 : value.find(',', c0 + 1);
                        if (c1 == std::string::npos) {
                            return USAGE;
                        }
                        const auto x = ParseFloat(value.substr(0, c0));
                        const auto y = ParseFloat(value.substr(c0 + 1, c1 - c0 - 1));
                        const auto z = ParseFloat(value.substr(c1 + 1));
                        if (!x || !y || !z) {
                            return USAGE;
                        }
                        settings.hasPosition = true;
                        settings.position[0] = *x;
                        settings.position[1] = *y;
                        settings.position[2] = *z;
                    } else if (key == "screen") {
                        const size_t c0 = value.find(',');
                        if (c0 == std::string::npos) {
                            return USAGE;
                        }
                        const auto x = ParseFloat(value.substr(0, c0));
                        const auto y = ParseFloat(value.substr(c0 + 1));
                        if (!x || !y) {
                            return USAGE;
                        }
                        settings.hasScreen = true;
                        settings.screen[0] = *x;
                        settings.screen[1] = *y;
                    } else {
                        return USAGE;
                    }
                }
                return add ? fx.Add(app, effect, settings) : fx.Start(app, effect, settings);
            });

        registry.Register("voxel.bricks", "round-trip synthetic grids and the latest voxel.scene result through the 8^3 brick form and report its size against the dense grid",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: voxel.bricks";
                }
                // The self-test report starts with its own OK/ERR; the overall verdict replaces it.
                const std::string selfTest = Render::Voxel::RunBrickOccupancySelfTest();
                bool ok = selfTest.rfind("OK", 0) == 0;
                std::string report = "synthetic " + selfTest.substr(selfTest.find(' ') + 1);
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (vox.HasResult()) {
                    std::string line;
                    const bool sceneOk = Render::Voxel::CheckBrickRoundTrip(vox.Grid(), line);
                    ok = ok && sceneOk;
                    report += std::string(" | scene ") + (sceneOk ? "OK " : "ERR ")
                        + (vox.Stats().fillMode == Render::Voxel::VoxelFillMode::Solid ? "solid " : "surface ") + line;
                } else {
                    report += " | scene: no result (run voxel.scene first)";
                }
                return std::string(ok ? "OK " : "ERR ") + report;
            });

        registry.Register("smoke.solid", "<0|1> [keep] - 1 resamples the latest voxel.scene grid onto the smoke grid and uses it as static obstacles (GPU Gems 3 ch.30); 0 removes them. Both clear the smoke field unless 'keep' is given with 1 (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty() || args.size() > 2 || (args[0] != "0" && args[0] != "1")
                    || (args.size() == 2 && (args[0] != "1" || args[1] != "keep"))) {
                    return "ERR usage: smoke.solid <0|1> [keep]";
                }
                const bool keepField = args.size() == 2;
                Render::Renderer& renderer = app.GetRenderer();
                if (args[0] == "0") {
                    renderer.ClearSmokeSolidMask();
                    return "OK solid=0";
                }
                const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
                if (!vox.HasResult()) {
                    return "ERR no voxel.scene result (run voxel.scene first)";
                }

                Render::Voxel::SimGridDesc sim;
                sim.origin[0] = renderer.GetSmokeOriginX();
                sim.origin[1] = renderer.GetSmokeOriginY();
                sim.origin[2] = renderer.GetSmokeOriginZ();
                sim.cellSize = renderer.GetSmokeCellSize();
                sim.countX = renderer.GetSmokeCountX();
                sim.countY = renderer.GetSmokeCountY();
                sim.countZ = renderer.GetSmokeCountZ();

                std::vector<uint32_t> words;
                const uint64_t solidCells = Render::Voxel::BuildSimGridSolidMask(vox.Desc(), vox.Grid(), sim, words);
                if (!renderer.SetSmokeSolidMask(words.data(), words.size(), !keepField)) {
                    return "ERR smoke sim not ready (enable smoke and let a frame run after resizing)";
                }
                const uint64_t totalCells = static_cast<uint64_t>(sim.countX) * sim.countY * sim.countZ;
                std::ostringstream out;
                out << std::fixed << "OK solid=1 cells=" << solidCells << "/" << totalCells
                    << " grid=" << sim.countX << "x" << sim.countY << "x" << sim.countZ
                    << " cell=" << std::setprecision(4) << sim.cellSize
                    << " voxel=" << vox.Desc().voxelSize
                    << " fill=" << (vox.Stats().fillMode == Render::Voxel::VoxelFillMode::Solid ? "solid" : "surface");
                return out.str();
            });

        registry.Register("smoke.solidtest", "run the voxel-grid to smoke-grid solid mask resampler self-test (CPU only)",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: smoke.solidtest";
                }
                return Render::Voxel::RunSimGridSolidMaskSelfTest();
            });

        registry.Register("voxel.view", "on|off [x|y|z] [proj|slice] [index] - show/hide the Voxel Occupancy window and pick its view",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: voxel.view on|off [x|y|z] [proj|slice] [index]";
                if (args.empty()) {
                    return USAGE;
                }
                Debug::VoxelOccupancyViewer& viewer = app.GetVoxelViewer();
                if (args[0] == "off") {
                    if (args.size() != 1) {
                        return USAGE;
                    }
                    viewer.SetVisible(false);
                    return "OK visible=0";
                }
                if (args[0] != "on" || args.size() > 4) {
                    return USAGE;
                }

                Render::Voxel::VoxelAxis axis = Render::Voxel::VoxelAxis::Y;
                Render::Voxel::OccupancyImageMode mode = Render::Voxel::OccupancyImageMode::Projection;
                int sliceIndex = 0;
                if (args.size() > 1) {
                    if (args[1] == "x") {
                        axis = Render::Voxel::VoxelAxis::X;
                    } else if (args[1] == "y") {
                        axis = Render::Voxel::VoxelAxis::Y;
                    } else if (args[1] == "z") {
                        axis = Render::Voxel::VoxelAxis::Z;
                    } else {
                        return USAGE;
                    }
                }
                if (args.size() > 2) {
                    if (args[2] == "proj") {
                        mode = Render::Voxel::OccupancyImageMode::Projection;
                    } else if (args[2] == "slice") {
                        mode = Render::Voxel::OccupancyImageMode::Slice;
                    } else {
                        return USAGE;
                    }
                }
                if (args.size() > 3) {
                    const auto value = ParseInt(args[3]);
                    if (!value || *value < 0) {
                        return USAGE;
                    }
                    sliceIndex = *value;
                }

                viewer.SetView(axis, mode, static_cast<uint32_t>(sliceIndex));
                viewer.SetVisible(true);

                const char* axisName = axis == Render::Voxel::VoxelAxis::X ? "x" : (axis == Render::Voxel::VoxelAxis::Y ? "y" : "z");
                std::ostringstream out;
                out << "OK visible=1 axis=" << axisName
                    << " mode=" << (mode == Render::Voxel::OccupancyImageMode::Projection ? "proj" : "slice")
                    << " slice=" << sliceIndex
                    << " hasResult=" << (app.GetSceneVoxelization().HasResult() ? 1 : 0);
                return out.str();
            });
    }
}
