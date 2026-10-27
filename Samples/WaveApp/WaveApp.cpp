#include "WaveApp.h"

#include "ApplicationCore.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Light/DefaultSunLight.h"
#include "ApplicationEntryPoint.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "Input/CameraInputBinding.h"
#include "Input/InputSystem.h"
#include "Object/StaticModel.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UIText.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Wave
{
    namespace
    {
        // Sea bed: 4.5 m deep around the origin, rising 1 in 4 toward +x; the waterline (bed = 0)
        // sits near x = 18 and wiggles with z so the surf line is not a straight edge.
        float SeaBedHeight(float x, float z)
        {
            const float slope = -4.5f + 0.25f * x + 0.5f * std::sin(0.17f * z) + 0.25f * std::sin(0.41f * z + 1.3f);
            return (std::max)(slope, -6.0f);
        }

        double NowMs()
        {
            using Clock = std::chrono::steady_clock;
            return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
        }

        constexpr float kOceanSize = 60.0f;
        constexpr float kFlipWaterColor[3] = { 0.03f, 0.18f, 0.24f };
        constexpr float kFlipMaxFrameDt = 1.0f / 30.0f;   // a slow frame does not make the tank take a huge step

        // Two rocks on the slope in front of the beach (foam b-rock). A breaks the surface by 0.8 m, B by 0.4 m.
        std::vector<FlipRock> MakeDefaultRocks()
        {
            FlipRock a;
            a.center[0] = 10.0f; a.center[1] = -2.0f; a.center[2] = 0.0f;
            a.radius[0] = 1.5f; a.radius[1] = 2.8f; a.radius[2] = 2.0f;
            FlipRock b;
            b.center[0] = 6.0f; b.center[1] = -3.0f; b.center[2] = -3.5f;
            b.radius[0] = 1.2f; b.radius[1] = 3.4f; b.radius[2] = 1.2f;
            return { a, b };
        }

        // The default tank: the FlipWaterSettings grid (x -12..20, the beach waterline near x = 18) with a
        // piston paddle on the x = -12 wall pushing waves up the slope.
        FlipWaterSettings MakeFlipSettings()
        {
            FlipWaterSettings settings;
            settings.paddleAmplitude = 0.8f;
            settings.paddlePeriod = 3.5f;
            settings.rocks = MakeDefaultRocks();
            return settings;
        }

        std::string FormatSelfTest(uint32_t passed, uint32_t total, const std::vector<std::string>& failures)
        {
            std::string out = (passed == total ? "OK" : "FAIL");
            out += " checks=" + std::to_string(passed) + "/" + std::to_string(total);
            for (const std::string& failure : failures) {
                out += " [" + failure + "]";
            }
            return out;
        }
    }

    void WaveApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("WaveApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("WaveApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }
        // Standing out at sea, looking toward the beach (+x) and slightly down.
        m_camera->SetTarget(-12.0f, 4.0f, -10.0f);
        m_camera->SetYawPitch(1.0f, 0.25f);
        m_camera->SetMoveSpeed(5.0f);
        m_camera->SetClipPlanes(0.0005f, 500.0f);

        app.UseDefaultRenderNodePreset();

        app.SetDirectionalLight(MakeDefaultSunLight());

        RebuildWindWaves();
        // The cluster DAG is built from the submitted vertices and would not see the in-place writes.
        app.SetUseMeshShader(false);

        m_beachModel = app.CreateStaticModel();
        m_waterModel = app.CreateStaticModel();
        m_foliageModel = app.CreateStaticModel();
        m_rockModel = app.CreateStaticModel();
        m_diffuseModel = app.CreateStaticModel();
        if (!m_beachModel || !m_waterModel || !m_foliageModel || !m_rockModel || !m_diffuseModel) {
            DebugLog("WaveApp: model creation failed.\n");
            app.RequestQuit();
            return;
        }
        BuildBeach();
        m_foliage.Configure(WindFoliageSettings{}, &SeaBedHeight);
        m_foliage.Update(m_wind, m_time);
        // Two-sided blades are separate triangles (WindFoliage), so the default back-face cull is fine.
        m_foliageModel->AddStaticMesh(m_foliage.GetMesh(), MakeOpaqueSurfaceMaterial(1.0f, 1.0f, 1.0f, 0.8f, 0.0f));
        m_ocean.Configure(OceanSurfaceSettings{}, &SeaBedHeight);
        m_ocean.Update(m_waves, m_time, 0.0f, &m_wind);
        m_flipSettings = MakeFlipSettings();
        BuildRocks();
        m_diffuseMesh.Configure();
        m_diffuseMesh.Update(m_flip, m_diffuse);
        {
            // Rough and faintly emissive: WaveApp has no ambient light, so foam in shadow would be black.
            SurfaceMaterial foam = MakeOpaqueSurfaceMaterial(1.0f, 1.0f, 1.0f, 0.9f, 0.0f);
            for (int c = 0; c < 3; ++c) {
                foam.emissive[c] = 0.5f;
            }
            m_diffuseModel->AddStaticMesh(m_diffuseMesh.GetMesh(), foam);
        }
        PushWaterMesh(app);

        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });

        RegisterCommands(app);
        ImGuiCoordinator::Instance().RegisterTab("Wave", "Wave", "Waves", [this, &app]() { DrawWaveTab(app); });
    }

    void WaveApp::BuildBeach()
    {
        const uint32_t n = 96;
        const uint32_t side = n + 1;
        const float step = kOceanSize / static_cast<float>(n);
        const float origin = -0.5f * kOceanSize;
        const float h = 0.05f;

        Mesh mesh;
        mesh.vertices.assign(side * side, Vertex{});
        mesh.indices.clear();
        for (uint32_t zi = 0; zi < side; ++zi) {
            for (uint32_t xi = 0; xi < side; ++xi) {
                const float x = origin + step * xi;
                const float z = origin + step * zi;
                Vertex& v = mesh.vertices[zi * side + xi];
                v.position[0] = x;
                v.position[1] = SeaBedHeight(x, z);
                v.position[2] = z;
                const float dx = (SeaBedHeight(x + h, z) - SeaBedHeight(x - h, z)) / (2.0f * h);
                const float dz = (SeaBedHeight(x, z + h) - SeaBedHeight(x, z - h)) / (2.0f * h);
                const float len = std::sqrt(dx * dx + 1.0f + dz * dz);
                v.normal[0] = -dx / len;
                v.normal[1] = 1.0f / len;
                v.normal[2] = -dz / len;
                // Wet sand below the waterline is darker.
                const float wet = v.position[1] < 0.1f ? 0.7f : 1.0f;
                v.color[0] = wet;
                v.color[1] = wet;
                v.color[2] = wet;
                v.color[3] = 1.0f;
                v.uv[0] = static_cast<float>(xi) / n;
                v.uv[1] = static_cast<float>(zi) / n;
            }
        }
        for (uint32_t z = 0; z < n; ++z) {
            for (uint32_t x = 0; x < n; ++x) {
                const uint32_t i0 = z * side + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + side;
                const uint32_t i3 = i2 + 1;
                mesh.indices.insert(mesh.indices.end(), { i0, i2, i1, i1, i2, i3 });
            }
        }
        m_beachModel->ClearModel();
        m_beachModel->AddStaticMesh(std::move(mesh), MakeOpaqueSurfaceMaterial(0.76f, 0.68f, 0.50f, 0.95f, 0.0f));
    }

    // The solver's rock shape: the upper half of the ellipsoid, then a vertical wall from its equator down to
    // cy - ry (inside the bed). Separate wall vertices keep the normals horizontal there.
    void WaveApp::BuildRocks()
    {
        m_rockModel->ClearModel();
        if (m_flipSettings.rocks.empty()) {
            return;
        }
        const uint32_t rings = 8;
        const uint32_t segments = 24;
        const float pi = 3.14159265f;
        Mesh mesh;
        auto addVertex = [&mesh](float x, float y, float z, float nx, float ny, float nz) {
            Vertex v{};
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = z;
            v.normal[0] = nx / len;
            v.normal[1] = ny / len;
            v.normal[2] = nz / len;
            for (int c = 0; c < 4; ++c) {
                v.color[c] = 1.0f;
            }
            mesh.vertices.push_back(v);
            return static_cast<uint32_t>(mesh.vertices.size() - 1);
        };
        // Wind each triangle so its face normal agrees with the vertex normals (outward).
        auto addTriangle = [&mesh](uint32_t a, uint32_t b, uint32_t c) {
            const float* pa = mesh.vertices[a].position;
            const float* pb = mesh.vertices[b].position;
            const float* pc = mesh.vertices[c].position;
            const float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
            const float e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
            const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float vn[3] = { 0.0f, 0.0f, 0.0f };
            for (uint32_t i : { a, b, c }) {
                for (int k = 0; k < 3; ++k) {
                    vn[k] += mesh.vertices[i].normal[k];
                }
            }
            if (n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] < 0.0f) {
                mesh.indices.insert(mesh.indices.end(), { a, b, c });
            } else {
                mesh.indices.insert(mesh.indices.end(), { a, c, b });
            }
        };
        for (const FlipRock& rock : m_flipSettings.rocks) {
            const float* c = rock.center;
            const float* r = rock.radius;
            // Dome: ring 0 is the equator, ring `rings` the top (one vertex per segment, degenerate but simple).
            const uint32_t dome = static_cast<uint32_t>(mesh.vertices.size());
            for (uint32_t i = 0; i <= rings; ++i) {
                const float lat = 0.5f * pi * static_cast<float>(i) / rings;
                for (uint32_t j = 0; j < segments; ++j) {
                    const float lon = 2.0f * pi * static_cast<float>(j) / segments;
                    const float ux = std::cos(lat) * std::cos(lon);
                    const float uy = std::sin(lat);
                    const float uz = std::cos(lat) * std::sin(lon);
                    addVertex(c[0] + r[0] * ux, c[1] + r[1] * uy, c[2] + r[2] * uz, ux / r[0], uy / r[1], uz / r[2]);
                }
            }
            for (uint32_t i = 0; i < rings; ++i) {
                for (uint32_t j = 0; j < segments; ++j) {
                    const uint32_t j1 = (j + 1) % segments;
                    const uint32_t i0 = dome + i * segments + j;
                    const uint32_t i1 = dome + i * segments + j1;
                    const uint32_t i2 = dome + (i + 1) * segments + j;
                    const uint32_t i3 = dome + (i + 1) * segments + j1;
                    addTriangle(i0, i1, i2);
                    if (i + 1 < rings) {
                        addTriangle(i1, i3, i2);
                    }
                }
            }
            const uint32_t wall = static_cast<uint32_t>(mesh.vertices.size());
            for (uint32_t j = 0; j < segments; ++j) {
                const float lon = 2.0f * pi * static_cast<float>(j) / segments;
                const float ux = std::cos(lon);
                const float uz = std::sin(lon);
                addVertex(c[0] + r[0] * ux, c[1], c[2] + r[2] * uz, ux / r[0], 0.0f, uz / r[2]);
                addVertex(c[0] + r[0] * ux, c[1] - r[1], c[2] + r[2] * uz, ux / r[0], 0.0f, uz / r[2]);
            }
            for (uint32_t j = 0; j < segments; ++j) {
                const uint32_t j1 = (j + 1) % segments;
                const uint32_t t0 = wall + 2 * j;
                const uint32_t t1 = wall + 2 * j1;
                addTriangle(t0, t0 + 1, t1);
                addTriangle(t1, t0 + 1, t1 + 1);
            }
        }
        m_rockModel->AddStaticMesh(std::move(mesh), MakeOpaqueSurfaceMaterial(0.32f, 0.30f, 0.28f, 0.9f, 0.0f));
    }

    void WaveApp::PushWaterMesh(ApplicationCore& app)
    {
        const double start = NowMs();
        m_waterModel->ClearModel();
        // Low roughness so the sun glints off the crests; foam whitens the vertex colour.
        SurfaceMaterial water = MakeOpaqueSurfaceMaterial(1.0f, 1.0f, 1.0f, 0.12f, 0.0f);
        // A faint teal glow stands in for light scattered inside the water. WaveApp has no ambient term, so the
        // FLIP tank's side walls, which lie in the shadow of the tank's own surface, rendered pure black.
        for (int c = 0; c < 3; ++c) {
            water.emissive[c] = kFlipWaterColor[c] * 0.6f;
        }
        m_waterModel->AddStaticMesh(CurrentWaterMesh(), water);
        // Full rebuild (ClearRenderObjects waits for the GPU); only for init and topology changes.
        app.InvalidateRenderObjects();
        m_waterMeshIndex = SIZE_MAX;
        m_foliageMeshIndex = SIZE_MAX;
        m_diffuseMeshIndex = SIZE_MAX;
        ++m_fullPushes;
        m_pushMs = static_cast<float>(NowMs() - start);
    }

    bool WaveApp::WriteWaterVertices(ApplicationCore& app)
    {
        return WriteMeshVertices(app, CurrentWaterMesh(), m_waterMeshIndex, &m_pushMs);
    }

    bool WaveApp::WriteMeshVertices(ApplicationCore& app, const Mesh& mesh, size_t& meshIndex, float* outMs)
    {
        Renderer& renderer = app.GetRenderer();
        if (meshIndex == SIZE_MAX) {
            // Identify the mesh among the submitted ones by its topology (beach, water and grass differ in size).
            const std::vector<Mesh>& meshes = renderer.GetSceneSubmitter().GetMeshes();
            for (size_t i = 0; i < meshes.size(); ++i) {
                if (meshes[i].vertices.size() == mesh.vertices.size() && meshes[i].indices == mesh.indices) {
                    meshIndex = i;
                    break;
                }
            }
            if (meshIndex == SIZE_MAX) {
                return false;
            }
        }
        const double start = NowMs();
        const bool ok = renderer.UpdateStaticMeshVertices(meshIndex, mesh.vertices.data(), mesh.vertices.size());
        *outMs = static_cast<float>(NowMs() - start);
        if (!ok) {
            meshIndex = SIZE_MAX;
        }
        return ok;
    }

    void WaveApp::UpdateFoliage(ApplicationCore& app)
    {
        if (!m_foliageOn) {
            return;
        }
        const double start = NowMs();
        m_foliage.Update(m_wind, m_time);
        m_foliageMs = static_cast<float>(NowMs() - start);
        // Same rule as the water: an unknown index waits for the next submit, a failed known write rebuilds.
        const bool known = m_foliageMeshIndex != SIZE_MAX;
        if (!WriteMeshVertices(app, m_foliage.GetMesh(), m_foliageMeshIndex, &m_foliagePushMs) && known) {
            PushWaterMesh(app);
        }
    }

    void WaveApp::UpdateDiffuseMesh(ApplicationCore& app)
    {
        // FLIP mode only. Leaving it rebuilds every model, which re-uploads the empty mesh added in OnInit.
        const double start = NowMs();
        m_diffuseMesh.Update(m_flip, m_diffuse);
        m_diffuseMeshMs = static_cast<float>(NowMs() - start);
        const bool known = m_diffuseMeshIndex != SIZE_MAX;
        if (!WriteMeshVertices(app, m_diffuseMesh.GetMesh(), m_diffuseMeshIndex, &m_diffusePushMs) && known) {
            PushWaterMesh(app);
        }
    }

    void WaveApp::RebuildWindWaves()
    {
        if (m_windWaves) {
            const CoastalWindSettings& w = m_wind.Settings();
            m_spectrumSettings.windSpeed = w.speed;
            m_spectrumSettings.windDirectionDeg = w.directionDeg;
            m_waves.SetWaves(BuildWindWaves(m_spectrumSettings, &m_spectrumInfo));
        } else {
            m_waves = GerstnerWaveSet::MakeDefault();
            m_spectrumInfo = {};
        }
        m_baseWaves = m_waves.Waves();
        std::vector<GerstnerWave>& waves = m_waves.MutableWaves();
        for (size_t i = 0; i < waves.size(); ++i) {
            waves[i].amplitude = m_baseWaves[i].amplitude * m_amplitudeScale;
            waves[i].steepness = m_baseWaves[i].steepness * m_steepnessScale;
        }
        m_windDirty = false;
    }

    void WaveApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
        if (!m_waterModel) {
            return;
        }
        if (m_windDirty) {
            RebuildWindWaves();
        }
        if (m_flipMode) {
            if (!m_paused) {
                m_time += deltaTime;   // the wind keeps blowing over the grass while the tank runs
            }
            UpdateFoliage(app);
            if (m_flipResetPending) {
                ResetFlip();
                m_flipResetPending = false;
            }
            if (!m_paused) {
                const double start = NowMs();
                DriveFlipPaddle();
                const float flipDt = (std::min)(deltaTime, kFlipMaxFrameDt);
                m_flip.Step(flipDt);
                m_flipMesh.Update(m_flip);
                m_flipMs = static_cast<float>(NowMs() - start);
                m_diffuse.Step(m_flip, flipDt);
            }
            UpdateDiffuseMesh(app);
            const bool known = m_waterMeshIndex != SIZE_MAX;
            if (!WriteWaterVertices(app) && known) {
                PushWaterMesh(app);
            }
            return;
        }
        if (m_resolutionDirty) {
            m_ocean.Configure(m_ocean.Settings(), &SeaBedHeight);
            m_resolutionDirty = false;
            m_ocean.Update(m_waves, m_time, 0.0f, &m_wind);
            PushWaterMesh(app);
            return;
        }
        const float dt = m_paused ? 0.0f : deltaTime;
        m_time += dt;
        const double start = NowMs();
        m_ocean.Update(m_waves, m_time, dt, &m_wind);
        m_updateMs = static_cast<float>(NowMs() - start);
        UpdateFoliage(app);
        // Before the first submit after a rebuild the mesh is not in the submitter yet; skip the write
        // rather than rebuilding again. A write that fails on a known index falls back to a rebuild.
        const bool known = m_waterMeshIndex != SIZE_MAX;
        if (!WriteWaterVertices(app) && known) {
            PushWaterMesh(app);
        }
    }

    void WaveApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void WaveApp::OnShutdown(ApplicationCore& app)
    {
        m_inputConnections.clear();
        app.ClearObjects();
        m_camera = nullptr;
        m_beachModel = nullptr;
        m_rockModel = nullptr;
        m_waterModel = nullptr;
        m_foliageModel = nullptr;
        m_diffuseModel = nullptr;
    }

    const Mesh& WaveApp::CurrentWaterMesh() const
    {
        return m_flipMode ? m_flipMesh.GetMesh() : m_ocean.GetMesh();
    }

    // Stage 5a: the Gerstner sea outside the tank drives its wave maker. A Gerstner point is a
    // Lagrangian water particle (Finch 2004 eq.9), so the time derivative of its displaced x is the
    // surface orbital velocity; it is averaged over the inlet columns and decays with depth as
    // exp(k y) (Airy deep water), k the amplitude-weighted mean wavenumber. Swell heading away from
    // +x pulls the wall back as well, so the piston follows the sea instead of a fixed cosine.
    void WaveApp::DriveFlipPaddle()
    {
        if (!m_flipSettings.paddleExternal) {
            return;
        }
        const FlipWaterSettings& s = m_flip.Settings();
        const float x = s.origin[0];
        constexpr float kEps = 1.0e-3f;
        double sum = 0.0;
        for (uint32_t k = 0; k < s.nz; ++k) {
            const float z = s.origin[2] + (static_cast<float>(k) + 0.5f) * s.cellSize;
            const GerstnerSample a = m_waves.Evaluate(x, z, m_time + kEps, 1.0f);
            const GerstnerSample b = m_waves.Evaluate(x, z, m_time - kEps, 1.0f);
            sum += (a.position[0] - b.position[0]) / (2.0f * kEps);
        }
        float weighted = 0.0f;
        float amplitude = 0.0f;
        for (const GerstnerWave& wave : m_waves.Waves()) {
            if (wave.wavelength > 0.0f) {
                weighted += wave.amplitude * (2.0f * 3.14159265f / wave.wavelength);
                amplitude += wave.amplitude;
            }
        }
        m_flipOceanDecay = amplitude > 0.0f ? weighted / amplitude : 0.0f;
        const float u = s.nz > 0 ? static_cast<float>(sum / s.nz) : 0.0f;
        m_flip.SetPaddleDrive(m_flipOceanGain * u, m_flipOceanDecay);
    }

    void WaveApp::ResetFlip()
    {
        m_flip.Reset(m_flipSettings, &SeaBedHeight);
        m_flipMesh.Configure(m_flip, kFlipWaterColor);
        m_diffuse.Reset();
    }

    void WaveApp::SetFlipMode(ApplicationCore& app, bool on)
    {
        if (on == m_flipMode) {
            return;
        }
        m_flipMode = on;
        if (on) {
            ResetFlip();
            m_flipResetPending = false;
        }
        // Topology changes between the ocean grid and the tank height field.
        PushWaterMesh(app);
    }

    std::string WaveApp::FlipStatusLine() const
    {
        const FlipWaterStats& st = m_flip.Stats();
        const FlipWaterSettings& s = m_flip.Settings();
        char buf[1280];
        std::snprintf(buf, sizeof(buf),
            "OK flip=%d t=%.2f grid=%ux%ux%u h=%.2f particles=%u fluid=%u unknowns=%u tall=%d tallColumns=%u "
            "quad=%d leaves=%u fineColumns=%u marked=%u maxDivCoarse=%.2e merged=%u substeps=%u iters=%u residual=%.2e converged=%d maxDiv=%.2e maxDivTall=%.2e maxSpeed=%.2f "
            "aol=%d band=%u..%u mean=%.2f aolI=%.3f "
            "surface=%.2f..%.2f paddle=%.2f/%.2f ocean=%d gain=%.2f paddleU=%.3f decayK=%.3f p2gMs=%.2f solveMs=%.2f g2pMs=%.2f stepMs=%.2f flipMs=%.2f "
            "rocks=%u pushMs=%.2f wetVerts=%u meshIndex=%d fullPushes=%u fps=%.1f",
            m_flipMode ? 1 : 0, st.time, s.nx, s.ny, s.nz, s.cellSize, st.particles, st.fluidCells, st.unknowns,
            s.tallCells ? 1 : 0, st.tallColumns,
            s.quadtree ? 1 : 0, st.quadLeaves, st.quadFineColumns, st.quadMarked, st.maxDivergenceCoarse, st.mergedCells, st.substeps, st.iterations, st.residual,
            st.converged ? 1 : 0, st.maxDivergence, st.maxDivergenceTall, st.maxSpeed,
            s.opticalLayers.enabled ? 1 : 0, st.opticalMinBand, st.opticalMaxBand, st.opticalMeanBand, st.opticalMaxIndicator,
            st.surfaceMin, st.surfaceMax,
            s.paddleAmplitude, s.paddlePeriod, s.paddleExternal ? 1 : 0, m_flipOceanGain, m_flip.PaddleDriveVelocity(),
            m_flipOceanDecay, st.p2gMs, st.solveMs, st.g2pMs, st.stepMs, m_flipMs, static_cast<uint32_t>(s.rocks.size()), m_pushMs,
            m_flipMesh.WetVertices(), m_waterMeshIndex == SIZE_MAX ? -1 : static_cast<int>(m_waterMeshIndex),
            m_fullPushes, ImGui::GetIO().Framerate);
        return buf;
    }

    std::string WaveApp::DiffuseStatusLine() const
    {
        const FlipDiffuseSettings& c = m_diffuse.Settings();
        const FlipDiffuseStats& st = m_diffuse.Stats();
        char buf[768];
        std::snprintf(buf, sizeof(buf),
            "OK diffuse=%d count=%u spray=%u foam=%u bubble=%u spawned=%u removed=%u surface=%u "
            "maxTa=%.3f maxWc=%.3f maxEk=%.3f dta=%.2f..%.2f dwc=%.2f..%.2f dek=%.2f..%.2f kta=%.1f kwc=%.1f "
            "life=%.1f..%.1f buoy=%.2f drag=%.2f max=%u diffMs=%.2f drawn=%u visible=%u size=%.2f meshMs=%.2f meshPushMs=%.2f meshIndex=%d",
            c.enabled ? 1 : 0, m_diffuse.Count(), st.spray, st.foam, st.bubble, st.spawned, st.removed,
            st.surfaceParticles, st.maxTrappedAir, st.maxCurvature, st.maxEnergy, c.trappedAirMin, c.trappedAirMax,
            c.waveCrestMin, c.waveCrestMax, c.energyMin, c.energyMax, c.trappedAirRate, c.waveCrestRate,
            c.lifetimeMin, c.lifetimeMax, c.buoyancy, c.drag, c.maxParticles, st.stepMs,
            m_diffuseMesh.Drawn(), m_diffuseMesh.Visible(), m_diffuseMesh.SizeScale(), m_diffuseMeshMs, m_diffusePushMs,
            m_diffuseMeshIndex == SIZE_MAX ? -1 : static_cast<int>(m_diffuseMeshIndex));
        return buf;
    }

    std::string WaveApp::DiffuseCommand(const std::vector<std::string>& args)
    {
        const std::string sub = args.empty() ? "status" : args[0];
        if (sub == "selftest") {
            std::vector<std::string> failures;
            uint32_t total = 0;
            const uint32_t passed = RunFlipDiffuseSelfTest(&failures, &total);
            return FormatSelfTest(passed, total, failures);
        }
        if (sub == "status") {
            return DiffuseStatusLine();
        }
        if (sub == "reset") {
            m_diffuse.Reset();
            return DiffuseStatusLine();
        }
        FlipDiffuseSettings& c = m_diffuse.MutableSettings();
        for (const std::string& arg : args) {
            const size_t eq = arg.find('=');
            if (eq == std::string::npos) {
                return "ERR expected key=value, got " + arg;
            }
            const std::string key = arg.substr(0, eq);
            const float f = std::strtof(arg.c_str() + eq + 1, nullptr);
            if (key == "on") {
                c.enabled = f != 0.0f;
            } else if (key == "dta") {
                c.trappedAirMin = (std::max)(f, 0.0f);
            } else if (key == "dtamax") {
                c.trappedAirMax = (std::max)(f, 0.0f);
            } else if (key == "dwc") {
                c.waveCrestMin = (std::max)(f, 0.0f);
            } else if (key == "dwcmax") {
                c.waveCrestMax = (std::max)(f, 0.0f);
            } else if (key == "dek") {
                c.energyMin = (std::max)(f, 0.0f);
            } else if (key == "dekmax") {
                c.energyMax = (std::max)(f, 0.0f);
            } else if (key == "kta") {
                c.trappedAirRate = (std::max)(f, 0.0f);
            } else if (key == "kwc") {
                c.waveCrestRate = (std::max)(f, 0.0f);
            } else if (key == "max") {
                c.maxParticles = static_cast<uint32_t>((std::max)(f, 0.0f));
            } else if (key == "life") {
                c.lifetimeMin = (std::max)(f, 0.0f);
            } else if (key == "lifemax") {
                c.lifetimeMax = (std::max)(f, 0.0f);
            } else if (key == "buoy") {
                c.buoyancy = f;
            } else if (key == "drag") {
                c.drag = std::clamp(f, 0.0f, 1.0f);
            } else if (key == "size") {
                m_diffuseMesh.SetSizeScale(std::clamp(f, 0.1f, 10.0f));
            } else {
                return "ERR unknown key " + key;
            }
        }
        return DiffuseStatusLine();
    }

    std::string WaveApp::FlipCommand(ApplicationCore& app, const std::vector<std::string>& args)
    {
        const std::string sub = args.empty() ? "status" : args[0];
        if (sub == "selftest") {
            std::vector<std::string> failures;
            uint32_t total = 0;
            const uint32_t passed = RunFlipWaterSelfTest(&failures, &total);
            return FormatSelfTest(passed, total, failures);
        }
        if (sub == "on" || sub == "off") {
            SetFlipMode(app, sub == "on");
            return FlipStatusLine();
        }
        if (sub == "status") {
            return FlipStatusLine();
        }
        // The rest change settings; key=value pairs (or "tall 0|1") apply on the next reset.
        bool reset = sub == "reset";
        for (size_t i = (reset ? 1 : 0); i < args.size(); ++i) {
            std::string key = args[i];
            std::string value;
            const size_t eq = key.find('=');
            if (eq != std::string::npos) {
                value = key.substr(eq + 1);
                key = key.substr(0, eq);
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                return "ERR expected key=value, got " + key;
            }
            const float f = std::strtof(value.c_str(), nullptr);
            if (key == "tall") {
                m_flipSettings.tallCells = f != 0.0f;
            } else if (key == "quad") {
                m_flipSettings.quadtree = f != 0.0f;
            } else if (key == "leaf") {
                m_flipSettings.sizing.maxLog2 = std::clamp(static_cast<uint32_t>(f), 0u, 6u);
            } else if (key == "vel") {
                m_flipSettings.sizing.velocityThreshold = (std::max)(f, 0.0f);
            } else if (key == "curv") {
                m_flipSettings.sizing.curvatureThreshold = (std::max)(f, 0.0f);
            } else if (key == "band") {
                m_flipSettings.bandCells = (std::max)(static_cast<uint32_t>(f), 1u);
            } else if (key == "aol") {
                m_flipSettings.opticalLayers.enabled = f != 0.0f;
            } else if (key == "aola") {
                m_flipSettings.opticalLayers.scale = (std::max)(f, 0.0f);
            } else if (key == "aolref") {
                m_flipSettings.opticalLayers.referenceSpeed = (std::max)(f, 1e-3f);
            } else if (key == "aolmin") {
                m_flipSettings.opticalLayers.minCells = (std::max)(static_cast<uint32_t>(f), 1u);
            } else if (key == "aolmax") {
                m_flipSettings.opticalLayers.maxCells = (std::max)(static_cast<uint32_t>(f), 1u);
            } else if (key == "smooth") {
                m_flipSettings.opticalLayers.smoothIterations = std::clamp(static_cast<uint32_t>(f), 0u, 8u);
            } else if (key == "paddle") {
                m_flipSettings.paddleAmplitude = (std::max)(f, 0.0f);
            } else if (key == "period") {
                m_flipSettings.paddlePeriod = (std::max)(f, 0.2f);
            } else if (key == "ocean") {
                m_flipSettings.paddleExternal = f != 0.0f;
            } else if (key == "gain") {
                m_flipOceanGain = (std::max)(f, 0.0f);
            } else if (key == "ppa") {
                m_flipSettings.particlesPerAxis = std::clamp(static_cast<uint32_t>(f), 1u, 3u);
            } else if (key == "flip") {
                m_flipSettings.flipRatio = std::clamp(f, 0.0f, 1.0f);
            } else if (key == "parallel") {
                m_flipSettings.parallel = f != 0.0f;
            } else if (key == "rock") {
                m_flipSettings.rocks = f != 0.0f ? MakeDefaultRocks() : std::vector<FlipRock>{};
                BuildRocks();
                PushWaterMesh(app);
            } else {
                return "ERR unknown key " + key;
            }
            reset = true;
        }
        if (!reset) {
            return "ERR usage: wave.flip selftest | on | off | status | reset [key=value] | tall 0|1 | quad 0|1 | leaf L | vel V | curv C | band N | "
                   "aol 0|1 | aola A | aolref V | aolmin N | aolmax N | smooth N | paddle A | period T | ocean 0|1 | gain G | ppa N | flip R | parallel 0|1 | rock 0|1";
        }
        if (m_flipMode) {
            ResetFlip();
            m_flipResetPending = false;
        }
        return FlipStatusLine();
    }

    std::string WaveApp::StatusLine() const
    {
        const OceanSurfaceStats& stats = m_ocean.Stats();
        const OceanSurfaceSettings& s = m_ocean.Settings();
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "OK t=%.2f paused=%d res=%u verts=%u amp=%.2f steep=%.2f minJ=%.3f maxH=%.2f foamVerts=%u "
            "foamj=%.2f decay=%.2f shore=%.2f updateMs=%.2f pushMs=%.2f meshIndex=%d fullPushes=%u fps=%.1f",
            m_time, m_paused ? 1 : 0, s.resolution, stats.vertices, m_amplitudeScale, m_steepnessScale,
            stats.minJacobian, stats.maxHeight, stats.foamVertices, s.foamJacobian, s.foamDecay, s.shoreFoamDepth,
            m_updateMs, m_pushMs, m_waterMeshIndex == SIZE_MAX ? -1 : static_cast<int>(m_waterMeshIndex), m_fullPushes,
            ImGui::GetIO().Framerate);
        return buf;
    }

    std::string WaveApp::ApplySetting(const std::string& key, const std::string& value)
    {
        const float f = std::strtof(value.c_str(), nullptr);
        OceanSurfaceSettings& s = m_ocean.MutableSettings();
        if (key == "amp" || key == "steep") {
            (key == "amp" ? m_amplitudeScale : m_steepnessScale) = (std::max)(f, 0.0f);
            std::vector<GerstnerWave>& waves = m_waves.MutableWaves();
            for (size_t i = 0; i < waves.size() && i < m_baseWaves.size(); ++i) {
                waves[i].amplitude = m_baseWaves[i].amplitude * m_amplitudeScale;
                waves[i].steepness = m_baseWaves[i].steepness * m_steepnessScale;
            }
        } else if (key == "foamj") {
            s.foamJacobian = f;
        } else if (key == "decay") {
            s.foamDecay = (std::max)(f, 0.01f);
        } else if (key == "shore") {
            s.shoreFoamDepth = (std::max)(f, 0.0f);
        } else if (key == "shoal") {
            s.shoalDepth = (std::max)(f, 0.01f);
        } else if (key == "res") {
            s.resolution = std::clamp(static_cast<uint32_t>(f), 8u, 512u);
            m_resolutionDirty = true;
        } else if (key == "pause") {
            m_paused = f != 0.0f;
        } else if (key == "time") {
            m_time = f;
        } else {
            return "unknown key " + key;
        }
        return "";
    }

    void WaveApp::RegisterCommands(ApplicationCore& app)
    {
        Debug::DebugCommandRegistry* registry = app.GetDebugCommandRegistry();
        if (!registry) {
            return;
        }
        // The lambdas capture this; WaveApp outlives the registry (ApplicationEntryPoint.h declares
        // the application before ApplicationCore).
        registry->Register("wave.status", "- Gerstner ocean state, foam counts and CPU timings",
            [this](const std::vector<std::string>&) -> std::string { return StatusLine(); });
        registry->Register("wave.set",
            "key=value ... - amp= steep= (scales of the default waves) foamj= decay= shore= shoal= res= pause=0|1 time=",
            [this](const std::vector<std::string>& args) -> std::string {
                for (const std::string& arg : args) {
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR expected key=value, got " + arg;
                    }
                    const std::string error = ApplySetting(arg.substr(0, eq), arg.substr(eq + 1));
                    if (!error.empty()) {
                        return "ERR " + error;
                    }
                }
                return StatusLine();
            });
        registry->Register("wave.selftest", "- analytic Gerstner checks (crest height, period, normals, Jacobian fold)",
            [](const std::vector<std::string>&) -> std::string {
                std::vector<std::string> failures;
                uint32_t total = 0;
                const uint32_t passed = RunGerstnerSelfTest(&failures, &total);
                return FormatSelfTest(passed, total, failures);
            });
        registry->Register("wave.wind",
            "status | selftest | [speed= dir= gust= gustlen= gustwidth= veer= fetch= waves=0|1 swell= ripples= ripplelen= rippleamp= "
            "foliage=0|1 stiff= maxbend= flutter=] - one coastal wind (Taylor 1938 frozen gusts) driving the JONSWAP swell "
            "(Hasselmann 1973 / Horvath 2015), gust ripples and Sousa 2007 grass bending",
            [this](const std::vector<std::string>& args) -> std::string { return WindCommand(args); });
        registry->Register("wave.flip",
            "selftest | on | off | status | reset [tall= quad= leaf= vel= curv= band= aol= aola= aolref= aolmin= aolmax= smooth= paddle= period= ppa= flip= parallel=] | tall 0|1 ... "
            "- FLIP water tank (Zhu & Bridson 2005), tall-cell pressure (Irving et al. 2006), "
            "quadtree columns (Narita et al. 2025, piecewise-constant leaves), "
            "adaptive optical layers (Narita & Kanai PG2026, aol=1)",
            [this, &app](const std::vector<std::string>& args) -> std::string { return FlipCommand(app, args); });
        registry->Register("wave.diffuse",
            "status | selftest | reset | [on= dta= dtamax= dwc= dwcmax= dek= dekmax= kta= kwc= max= life= lifemax= buoy= drag=] "
            "- spray/foam/bubble particles on the FLIP tank (Ihmsen, Akinci, Akinci & Teschner 2012); not drawn yet",
            [this](const std::vector<std::string>& args) -> std::string { return DiffuseCommand(args); });
    }

    std::string WaveApp::WindStatusLine() const
    {
        const CoastalWindSettings& w = m_wind.Settings();
        const WindFoliageStats& f = m_foliage.Stats();
        char buf[768];
        std::snprintf(buf, sizeof(buf),
            "OK speed=%.2f dir=%.1f gust=%.2f gustlen=%.1f gustwidth=%.1f veer=%.1f waves=%d fetch=%.0f swell=%u ripples=%u "
            "peakLambda=%.2f Hs=%.3f HsEmpirical=%.3f alpha=%.5f oceanMeanGust=%.3f "
            "foliage=%d blades=%u bladeVerts=%u meanBend=%.3f maxBend=%.3f grassMeanGust=%.3f foliageMs=%.2f foliagePushMs=%.2f "
            "foliageIndex=%d t=%.2f",
            w.speed, w.directionDeg, w.gustStrength, w.gustLength, w.gustWidth, w.gustVeerDeg, m_windWaves ? 1 : 0,
            m_spectrumSettings.fetch, m_spectrumSettings.swellWaves, m_spectrumSettings.rippleWaves,
            m_spectrumInfo.peakWavelength, m_spectrumInfo.significantHeight, m_spectrumInfo.empiricalHeight,
            m_spectrumInfo.alpha, m_ocean.Stats().meanGust,
            m_foliageOn ? 1 : 0, f.blades, f.vertices, f.meanBend, f.maxBend, f.meanGust, m_foliageMs, m_foliagePushMs,
            m_foliageMeshIndex == SIZE_MAX ? -1 : static_cast<int>(m_foliageMeshIndex), m_time);
        return buf;
    }

    std::string WaveApp::WindCommand(const std::vector<std::string>& args)
    {
        if (!args.empty() && args[0] == "selftest") {
            std::vector<std::string> failures;
            uint32_t total = 0;
            uint32_t passed = 0;
            uint32_t part = 0;
            passed += RunCoastalWindSelfTest(&failures, &part);
            total += part;
            passed += RunWindWaveSpectrumSelfTest(&failures, &part);
            total += part;
            passed += RunWindFoliageSelfTest(&failures, &part);
            total += part;
            return FormatSelfTest(passed, total, failures);
        }
        CoastalWindSettings& w = m_wind.MutableSettings();
        WindFoliageSettings& fs = m_foliage.MutableSettings();
        for (const std::string& arg : args) {
            if (arg == "status") {
                continue;
            }
            const size_t eq = arg.find('=');
            if (eq == std::string::npos) {
                return "ERR expected key=value, got " + arg;
            }
            const std::string key = arg.substr(0, eq);
            const float f = std::strtof(arg.c_str() + eq + 1, nullptr);
            if (key == "speed") {
                w.speed = std::clamp(f, 0.0f, 40.0f);
                m_windDirty = true;
            } else if (key == "dir") {
                w.directionDeg = f;
                m_windDirty = true;
            } else if (key == "gust") {
                w.gustStrength = std::clamp(f, 0.0f, 1.0f);
            } else if (key == "gustlen") {
                w.gustLength = (std::max)(f, 0.5f);
            } else if (key == "gustwidth") {
                w.gustWidth = (std::max)(f, 0.5f);
            } else if (key == "veer") {
                w.gustVeerDeg = std::clamp(f, 0.0f, 90.0f);
            } else if (key == "waves") {
                m_windWaves = f != 0.0f;
                m_windDirty = true;
            } else if (key == "fetch") {
                m_spectrumSettings.fetch = std::clamp(f, 100.0f, 1.0e6f);
                m_windDirty = true;
            } else if (key == "swell") {
                m_spectrumSettings.swellWaves = std::clamp(static_cast<uint32_t>(f), 0u, 8u);
                m_windDirty = true;
            } else if (key == "ripples") {
                m_spectrumSettings.rippleWaves = std::clamp(static_cast<uint32_t>(f), 0u, 4u);
                m_windDirty = true;
            } else if (key == "ripplelen") {
                m_spectrumSettings.rippleWavelength = (std::max)(f, 0.2f);
                m_windDirty = true;
            } else if (key == "rippleamp") {
                m_spectrumSettings.rippleAmplitude = (std::max)(f, 0.0f);
                m_windDirty = true;
            } else if (key == "foliage") {
                m_foliageOn = f != 0.0f;
            } else if (key == "stiff") {
                fs.stiffness = (std::max)(f, 0.0f);
            } else if (key == "maxbend") {
                fs.maxBend = (std::max)(f, 0.0f);
            } else if (key == "flutter") {
                fs.flutter = (std::max)(f, 0.0f);
            } else {
                return "ERR unknown key " + key;
            }
        }
        if (m_windDirty) {
            RebuildWindWaves();
        }
        return WindStatusLine();
    }

    void WaveApp::DrawWindSection()
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Wind (drives waves + grass)");
        CoastalWindSettings& w = m_wind.MutableSettings();
        m_windDirty |= ImGui::SliderFloat("Wind speed (m/s)", &w.speed, 0.0f, 25.0f);
        m_windDirty |= ImGui::SliderFloat("Wind direction (deg)", &w.directionDeg, -180.0f, 180.0f);
        ImGui::SliderFloat("Gust strength", &w.gustStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Gust length (m)", &w.gustLength, 2.0f, 40.0f);
        m_windDirty |= ImGui::Checkbox("Wind-driven waves", &m_windWaves);
        ImGui::SameLine();
        ImGui::Checkbox("Grass", &m_foliageOn);
        if (m_windWaves) {
            m_windDirty |= ImGui::SliderFloat("Fetch (m)", &m_spectrumSettings.fetch, 500.0f, 50000.0f, "%.0f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::Text("peak lambda %.2f m  Hs %.2f m (growth law %.2f)", m_spectrumInfo.peakWavelength,
                m_spectrumInfo.significantHeight, m_spectrumInfo.empiricalHeight);
        }
        const WindFoliageStats& f = m_foliage.Stats();
        ImGui::Text("grass %u blades  bend %.2f (max %.2f)  %.2f ms", f.blades, f.meanBend, f.maxBend, m_foliageMs);
    }

    void WaveApp::DrawWaveTab(ApplicationCore& app)
    {
        bool flip = m_flipMode;
        if (ImGui::Checkbox("FLIP tank (stage 2)", &flip)) {
            SetFlipMode(app, flip);
        }
        if (m_flipMode) {
            const FlipWaterStats& st = m_flip.Stats();
            ImGui::Text("FPS: %.1f  step %.2f ms (solve %.2f)  push %.2f ms", ImGui::GetIO().Framerate, m_flipMs,
                st.solveMs, m_pushMs);
            ImGui::Text("particles %u  unknowns %u  iters %u  substeps %u", st.particles, st.unknowns, st.iterations,
                st.substeps);
            ImGui::Text("tall columns %u  merged cells %u  max |div| %.1e", st.tallColumns, st.mergedCells,
                st.maxDivergence);
            if (m_flipSettings.quadtree) {
                ImGui::Text("quad leaves %u  fine columns %u  marked %u  coarse |div| %.1e", st.quadLeaves,
                    st.quadFineColumns, st.quadMarked, st.maxDivergenceCoarse);
            }
            if (m_flipSettings.opticalLayers.enabled) {
                ImGui::Text("optical band %u..%u  mean %.2f  max I %.3f", st.opticalMinBand, st.opticalMaxBand,
                    st.opticalMeanBand, st.opticalMaxIndicator);
            }
            ImGui::Checkbox("Pause", &m_paused);
            m_flipResetPending |= ImGui::Checkbox("Tall cells", &m_flipSettings.tallCells);
            ImGui::SameLine();
            m_flipResetPending |= ImGui::Checkbox("Quadtree columns", &m_flipSettings.quadtree);
            ImGui::SameLine();
            m_flipResetPending |= ImGui::Checkbox("Optical layers", &m_flipSettings.opticalLayers.enabled);
            m_flipResetPending |= ImGui::SliderFloat("Paddle (m/s)", &m_flipSettings.paddleAmplitude, 0.0f, 2.0f);
            m_flipResetPending |= ImGui::SliderFloat("Paddle period (s)", &m_flipSettings.paddlePeriod, 1.0f, 8.0f);
            m_flipResetPending |= ImGui::Checkbox("Ocean drives paddle", &m_flipSettings.paddleExternal);
            if (m_flipSettings.paddleExternal) {
                ImGui::SameLine();
                ImGui::SliderFloat("Gain", &m_flipOceanGain, 0.0f, 3.0f);
                ImGui::Text("paddle u %.2f m/s  decay k %.3f /m", m_flip.PaddleDriveVelocity(), m_flipOceanDecay);
            }
            if (ImGui::Button("Reset tank")) {
                m_flipResetPending = true;
            }
            ImGui::Checkbox("Spray/foam/bubbles (Ihmsen 2012)", &m_diffuse.MutableSettings().enabled);
            {
                const FlipDiffuseStats& ds = m_diffuse.Stats();
                ImGui::Text("diffuse %u: spray %u foam %u bubble %u  +%u -%u  %.2f ms", m_diffuse.Count(), ds.spray,
                    ds.foam, ds.bubble, ds.spawned, ds.removed, ds.stepMs);
            }
            UI::TextDisabled("CPU FLIP, height-field surface. Diffuse particles are simulated, not drawn yet. Changes reset the tank.");
            DrawWindSection();
            return;
        }
        DrawWindSection();
        ImGui::Separator();
        const OceanSurfaceStats& stats = m_ocean.Stats();
        ImGui::Text("FPS: %.1f  update %.2f ms  push %.2f ms", ImGui::GetIO().Framerate, m_updateMs, m_pushMs);
        ImGui::Text("min J %.3f  foam verts %u / %u", stats.minJacobian, stats.foamVertices, stats.vertices);
        ImGui::Checkbox("Pause", &m_paused);

        ImGui::Separator();
        float amp = m_amplitudeScale;
        if (ImGui::SliderFloat("Amplitude", &amp, 0.0f, 2.5f)) {
            ApplySetting("amp", std::to_string(amp));
        }
        float steep = m_steepnessScale;
        if (ImGui::SliderFloat("Steepness", &steep, 0.0f, 2.0f)) {
            ApplySetting("steep", std::to_string(steep));
        }

        ImGui::Separator();
        OceanSurfaceSettings& s = m_ocean.MutableSettings();
        ImGui::SliderFloat("Foam J threshold", &s.foamJacobian, 0.0f, 1.0f);
        ImGui::SliderFloat("Foam decay (s)", &s.foamDecay, 0.05f, 6.0f);
        ImGui::SliderFloat("Shore foam depth (m)", &s.shoreFoamDepth, 0.0f, 2.0f);
        ImGui::SliderFloat("Shoal depth (m)", &s.shoalDepth, 0.1f, 8.0f);
        int res = static_cast<int>(s.resolution);
        if (ImGui::SliderInt("Resolution", &res, 16, 256)) {
            ApplySetting("res", std::to_string(res));
        }
        UI::TextDisabled("CPU Gerstner mesh, vertices written in place each frame (no GPU ring yet).");
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::Wave::WaveApp, 1280, 720, L"Wave Sample")
