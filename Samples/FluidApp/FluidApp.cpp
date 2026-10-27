#include "FluidApp.h"
#include "ApplicationCore.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Object/SampleFloorBox.h"
#include "Light/DefaultSunLight.h"
#include "ApplicationEntryPoint.h"
#include "AppFramework/Debug/VoxelOccupancyViewer.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "Input/CameraInputBinding.h"
#include "Input/InputSystem.h"
#include "Object/StaticModel.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Runtime/Renderer.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UIButton.h"
#include "UI/UIIntVolume.h"
#include "UI/UIText.h"
#include "UI/UIVolume.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Fluid
{
    namespace
    {
        constexpr float kBistroOffsetX = 60.0f;
    }

    void FluidApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("FluidApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("FluidApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        m_camera->SetTarget(m_camX, m_camY, m_camZ);
        m_camera->SetYawPitch(m_yaw, m_pitch);
        m_camera->SetMoveSpeed(5.0f);
        m_camera->SetClipPlanes(0.0005f, 500.0f);

        app.UseDefaultRenderNodePreset();

        StaticModel* floorModel = app.CreateStaticModel();
        if (!floorModel) {
            DebugLog("FluidApp: Floor model creation failed.\n");
            app.RequestQuit();
            return;
        }

        AddSampleFloorBox(*floorModel, MakeOpaqueSurfaceMaterial(0.52f, 0.54f, 0.58f, 0.9f, 0.0f));

        app.FitFluidToSceneBounds(-6.0f, -6.0f, 6.0f, 6.0f, 0.0f);
        // Enabled by default: this app's whole purpose is demoing the fluid system.
        // The floor box is 0.2 tall centred at y = -0.1, so its top face is exactly y = 0. Leaving
        // the water there put an alpha-blended surface coplanar with an opaque one, which z-fights.
        app.SetFluidSurfaceHeight(0.05f);
        app.SetFluidSurfaceEnabled(true);

        // Set up directional light (sun)
        app.SetDirectionalLight(MakeDefaultSunLight());

        // Input bindings
        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });

        RegisterBistroCommand(app);

        // UI window: one registered tab per page of the "Fluid" window.
        ImGuiCoordinator& ui = ImGuiCoordinator::Instance();
        ui.RegisterTab("Fluid", "Fluid", "Water", [this, &app]() { DrawWaterTab(app); });
        ui.RegisterTab("Fluid", "Fluid", "Smoke", [this, &app]() { DrawSmokeTab(app); });
        // Voxel occupancy and the wind flow overlay live here rather than in PBRApp:
        // the projected wind is a fluid solve, so it belongs to the fluid sample.
        ui.RegisterTab("Fluid", "Fluid", "Voxel / Wind", [this, &app]() {
            DrawBistroControls(app);
            ImGui::Separator();
            UI::TextDisabled("1-bit occupancy of opaque static geometry (DX12 only).");
            app.GetVoxelViewer().DrawContents(app);
        });
    }

    void FluidApp::DrawWaterTab(ApplicationCore& app)
    {
        if (!m_camera) { ImGui::TextDisabled("Camera unavailable."); return; }

        // FPS
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

        // Camera position
        const auto& pos = m_camera->Transform().position;
        ImGui::Text("Pos: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

        ImGui::Separator();

        float speed = m_camera->MoveSpeed();
        if (UI::Volume("Move Speed", &speed, 0.1f, 30.0f)) {
            m_camera->SetMoveSpeed(speed);
        }

        ImGui::Separator();
        float waveSpeed = app.GetFluidWaveSpeed();
        if (UI::Volume("Wave Speed", &waveSpeed, 0.5f, 20.0f)) {
            app.SetFluidWaveSpeed(waveSpeed);
        }
        float damping = app.GetFluidDamping();
        if (UI::Volume("Damping", &damping, 0.9f, 1.0f)) {
            app.SetFluidDamping(damping);
        }
        if (ImGui::Button("Inject Splash")) {
            app.InjectFluidSplash(0.0f, 0.0f, 1.0f, 2.0f);
        }
    }

    void FluidApp::DrawSmokeTab(ApplicationCore& app)
    {
        // Same knobs as the smoke.* debug commands; they stay available in every app.
        Renderer& renderer = app.GetRenderer();
        UI::TextDisabled("Grid-based smoke (GPU Gems ch.38). Its buffers are upload heaps, DX12 only.");

        bool enabled = renderer.GetSmokeEnabled();
        if (ImGui::Checkbox("Enable smoke (smoke)", &enabled)) {
            renderer.SetSmokeEnabled(enabled);
        }
        bool emitter = renderer.GetSmokeEmitterActive();
        if (ImGui::Checkbox("Emitter active", &emitter)) {
            renderer.SetSmokeEmitterActive(emitter);
        }

        int buoyancyMode = static_cast<int>(renderer.GetSmokeBuoyancyMode());
        bool modeChanged = ImGui::RadioButton("Buoyancy: density", &buoyancyMode, 0);
        ImGui::SameLine();
        modeChanged |= ImGui::RadioButton("temperature (Fedkiw 2001)", &buoyancyMode, 1);
        if (modeChanged) {
            renderer.SetSmokeBuoyancyMode(static_cast<SmokeVolumeSim::BuoyancyMode>(buoyancyMode));
        }

        float vorticity = renderer.GetSmokeVorticity();
        if (UI::Volume("Vorticity", &vorticity, 0.0f, 10.0f)) {
            renderer.SetSmokeVorticity(vorticity);
        }
        int jacobi = static_cast<int>(renderer.GetSmokeJacobiIterations());
        if (UI::IntVolume("Jacobi iterations", &jacobi, 1, 200)) {
            renderer.SetSmokeJacobiIterations(static_cast<uint32_t>(jacobi));
        }

        // The sim normalises the direction and has no getter for it, so the panel keeps its own copy.
        float windStrength = renderer.GetSmokeWindStrength();
        bool windChanged = ImGui::DragFloat3("Wind direction", m_smokeWindDir, 0.01f, -1.0f, 1.0f);
        windChanged |= UI::Volume("Wind strength", &windStrength, 0.0f, 20.0f);
        if (windChanged) {
            renderer.SetSmokeWind(m_smokeWindDir[0], m_smokeWindDir[1], m_smokeWindDir[2], windStrength);
        }

        ImGui::Separator();
        UI::Text("Grid %u x %u x %u  cell %.3f",
                 renderer.GetSmokeCountX(),
                 renderer.GetSmokeCountY(),
                 renderer.GetSmokeCountZ(),
                 renderer.GetSmokeCellSize());
        UI::Text("Origin (%.2f, %.2f, %.2f)",
                 renderer.GetSmokeOriginX(),
                 renderer.GetSmokeOriginY(),
                 renderer.GetSmokeOriginZ());
        UI::Text("Solid obstacles: %s (smoke.solid 1, needs voxel.scene)",
                 renderer.IsSmokeSolidActive() ? "on" : "off");
    }

    void FluidApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        (void)app;
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void FluidApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void FluidApp::OnShutdown(ApplicationCore& app)
    {
        m_inputConnections.clear();
        app.ClearObjects();
        m_camera = nullptr;
        m_bistroModel = nullptr;
    }

    bool FluidApp::LoadBistro(ApplicationCore& app, std::string& error)
    {
        if (m_bistroModel) {
            return true;
        }
        StaticModel* model = app.CreateStaticModel();
        if (!model) {
            error = "static model creation failed";
            return false;
        }
        // Synchronous, as in PBRApp: parsing this FBX on a worker thread kills the process.
        if (!model->LoadModel(BISTRO_EXTERIOR_PATH, StaticModel::ModelFormat::Fbx, 1.0f)) {
            app.DeleteObject(model);
            error = std::string("load failed: Assets/") + BISTRO_EXTERIOR_PATH;
            return false;
        }
        // Same +X offset as PBRApp, which also keeps the water pool at the origin clear.
        model->SetTranslation(kBistroOffsetX, 0.0f, 0.0f);
        m_bistroModel = model;
        return true;
    }

    void FluidApp::DrawBistroControls(ApplicationCore& app)
    {
        if (m_bistroModel) {
            UI::Text("Bistro loaded at x=%.0f (fluid.bistro)", kBistroOffsetX);
            return;
        }
        if (UI::Button("Load Bistro")) {
            m_bistroError.clear();
            LoadBistro(app, m_bistroError);
        }
        ImGui::SameLine();
        UI::TextDisabled("blocks a few seconds; obstacles for voxel/wind/smoke");
        if (!m_bistroError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_bistroError.c_str());
        }
    }

    void FluidApp::RegisterBistroCommand(ApplicationCore& app)
    {
        Debug::DebugCommandRegistry* registry = app.GetDebugCommandRegistry();
        if (!registry) {
            return;
        }
        // The lambda captures this; FluidApp outlives the registry (ApplicationEntryPoint.h
        // declares the application before ApplicationCore).
        registry->Register("fluid.bistro",
            "- load the Bistro exterior (sync) at x=60 as fluid/wind obstacles; no-op if already loaded",
            [this, &app](const std::vector<std::string>&) -> std::string {
                const bool already = m_bistroModel != nullptr;
                std::string error;
                if (!LoadBistro(app, error)) {
                    return "ERR " + error;
                }
                return already ? "OK bistro already loaded" : "OK bistro loaded offsetX=60";
            });
    }

}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::Fluid::FluidApp, 1280, 720, L"Fluid Sample")
