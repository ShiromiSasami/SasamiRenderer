#include "NeuralShaderApp.h"
#include "ApplicationCore.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Object/SampleFloorBox.h"
#include "Light/DefaultSunLight.h"
#include "ApplicationEntryPoint.h"
#include "Input/CameraInputBinding.h"
#include "Input/InputSystem.h"
#include "Object/StaticModel.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UIVolume.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <windows.h>
#include <algorithm>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::NeuralShader
{
    namespace
    {
    }

    void NeuralShaderApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("NeuralShaderApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("NeuralShaderApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        m_camera->SetTarget(m_camX, m_camY, m_camZ);
        m_camera->SetYawPitch(m_yaw, m_pitch);
        m_camera->SetMoveSpeed(4.0f);

        app.UseDefaultRenderNodePreset();

        // Set up directional light (sun)
        app.SetDirectionalLight(MakeDefaultSunLight());

        StaticModel* floorModel = app.CreateStaticModel();
        if (!floorModel) {
            DebugLog("NeuralShaderApp: Floor object creation failed.\n");
            app.RequestQuit();
            return;
        }
        AddSampleFloorBox(*floorModel, MakeOpaqueSurfaceMaterial(0.52f, 0.54f, 0.58f, 0.9f, 0.0f));

        StaticModel* boxModel = app.CreateStaticModel();
        if (!boxModel) {
            DebugLog("NeuralShaderApp: Box object creation failed.\n");
            app.RequestQuit();
            return;
        }
        StaticModel::BoxDesc boxDesc{};
        boxDesc.width    = 1.2f;
        boxDesc.height   = 1.2f;
        boxDesc.depth    = 1.2f;
        boxDesc.material = MakeOpaqueSurfaceMaterial(0.75f, 0.35f, 0.85f, 0.3f, 0.2f);
        boxModel->AddBox(boxDesc);
        boxModel->SetTranslation(0.0f, 0.6f, 0.5f);

        // Input bindings
        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });

        // UI window
        ImGuiCoordinator::Instance().RegisterWindow("Neural Shader", [this]() {
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
        });
    }

    void NeuralShaderApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        (void)app;
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void NeuralShaderApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void NeuralShaderApp::OnShutdown(ApplicationCore& app)
    {
        m_inputConnections.clear();
        app.ClearObjects();
        m_camera = nullptr;
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::NeuralShader::NeuralShaderApp, 1280, 720, L"Neural Shader Sample")
