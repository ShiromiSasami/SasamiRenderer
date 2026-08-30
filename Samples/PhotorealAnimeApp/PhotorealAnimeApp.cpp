#include "PhotorealAnimeApp.h"
#include "ApplicationCore.h"
#include "ApplicationEntryPoint.h"
#include "Input/InputSystem.h"
#include "Object/StaticModel.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "UI/ImGuiCoordinator.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <boost/signals2/connection.hpp>

namespace SasamiRenderer
{
    namespace
    {
        SurfaceMaterial MakeMaterial(float r, float g, float b, float roughness, float metallic)
        {
            SurfaceMaterial material{};
            material.baseColor[0] = r;
            material.baseColor[1] = g;
            material.baseColor[2] = b;
            material.baseColor[3] = 1.0f;
            material.roughness = roughness;
            material.metallic = metallic;
            material.reflectionStrength = Math::DefaultReflectionStrength(roughness, metallic);
            material.occlusionStrength = 1.0f;
            return material;
        }
    }

    void PhotorealAnimeApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("PhotorealAnimeApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("PhotorealAnimeApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        m_camera->SetTarget(m_camX, m_camY, m_camZ);
        m_camera->SetYawPitch(m_yaw, m_pitch);
        m_camera->SetMoveSpeed(4.0f);

        app.UseDefaultRenderNodePreset();

        StaticModel* floorModel = app.CreateStaticModel();
        if (!floorModel) {
            DebugLog("PhotorealAnimeApp: Floor model creation failed.\n");
            app.RequestQuit();
            return;
        }
        StaticModel::BoxDesc floorDesc{};
        floorDesc.width = 12.0f;
        floorDesc.height = 0.2f;
        floorDesc.depth = 12.0f;
        floorDesc.material = MakeMaterial(0.52f, 0.54f, 0.58f, 0.9f, 0.0f);
        floorModel->AddBox(floorDesc);
        floorModel->SetTranslation(0.0f, -0.1f, 0.5f);

        StaticModel* boxModel = app.CreateStaticModel();
        if (!boxModel) {
            DebugLog("PhotorealAnimeApp: Box model creation failed.\n");
            app.RequestQuit();
            return;
        }
        StaticModel::BoxDesc boxDesc{};
        boxDesc.width = 1.2f;
        boxDesc.height = 1.2f;
        boxDesc.depth = 1.2f;
        boxDesc.material = MakeMaterial(0.90f, 0.65f, 0.75f, 0.4f, 0.0f);
        boxModel->AddBox(boxDesc);
        boxModel->SetTranslation(0.0f, 0.6f, 0.5f);

        // Set up directional light (sun)
        DirectionalLight sun{};
        sun.yaw       = 45.0f;
        sun.pitch     = 40.0f;
        sun.intensity = 2.5f;
        sun.color.r = 1.00f;
        sun.color.g = 0.95f;
        sun.color.b = 0.85f;
        app.SetDirectionalLight(sun);

        // Input bindings
        auto& input = InputSystem::Instance();
        static std::vector<boost::signals2::scoped_connection> s_connections;
        s_connections.clear();

        s_connections.emplace_back(input.ConnectOnKeyDown([this](WPARAM k) {
            if (m_camera) m_camera->OnKeyDown(k);
        }));
        s_connections.emplace_back(input.ConnectOnKeyUp([this](WPARAM k) {
            if (m_camera) m_camera->OnKeyUp(k);
        }));
        s_connections.emplace_back(input.ConnectOnMouseDown([this](int x, int y) {
            if (m_camera) m_camera->OnMouseDown(x, y);
        }));
        s_connections.emplace_back(input.ConnectOnMouseUp([this]() {
            if (m_camera) m_camera->OnMouseUp();
        }));
        s_connections.emplace_back(input.ConnectOnMouseMove([this](int x, int y, bool held) {
            if (m_camera) m_camera->OnMouseMove(x, y, held);
        }));
        s_connections.emplace_back(input.ConnectOnMouseWheel([this](int d) {
            if (m_camera) m_camera->OnMouseWheel(d);
        }));

        // UI window
        ImGuiCoordinator::Instance().RegisterWindow("Photoreal Anime Environment", [this]() {
            if (!m_camera) { ImGui::TextDisabled("Camera unavailable."); return; }

            // FPS
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

            // Camera position
            const auto& pos = m_camera->Transform().position;
            ImGui::Text("Pos: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

            ImGui::Separator();

            float speed = m_camera->MoveSpeed();
            if (ImGui::SliderFloat("Move Speed", &speed, 0.1f, 30.0f)) {
                m_camera->SetMoveSpeed(speed);
            }
        });
    }

    void PhotorealAnimeApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        (void)app;
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void PhotorealAnimeApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void PhotorealAnimeApp::OnShutdown(ApplicationCore& app)
    {
        app.ClearObjects();
        m_camera = nullptr;
    }

    void PhotorealAnimeApp::OnResize(ApplicationCore& app, UINT width, UINT height)
    {
        (void)app;
        (void)width;
        (void)height;
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiRenderer::PhotorealAnimeApp, 1280, 720, L"Photoreal Anime Sample")
