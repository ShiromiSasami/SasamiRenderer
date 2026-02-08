#include "RenderingApp.h"
#include "Application.h"
#include "Renderer.h"
#include "Object/SModelObject.h"
#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"
#include "imgui.h"

namespace SasamiRenderer
{
    void RenderingApp::OnInit(Application& app)
    {
        m_renderer = std::make_unique<Renderer>();
        if (!m_renderer->Initialize(app.GetHwnd(), app.GetWidth(), app.GetHeight())) {
            app.RequestQuit();
            return;
        }

        std::vector<Renderer::RenderProxy> rendererProxies;
        auto appendProxies = [&rendererProxies](SModelObject&& obj) {
            auto objectProxies = obj.BuildRenderProxies();
            rendererProxies.reserve(rendererProxies.size() + objectProxies.size());
            for (auto& proxy : objectProxies) {
                rendererProxies.push_back(std::move(proxy));
            }
        };

        SModelObject bunny;
        if (!bunny.LoadModel(L"Models\\Bunny.obj", SModelObject::ModelFormat::Obj)) {
            app.RequestQuit();
            return;
        }
        appendProxies(std::move(bunny));

        SModelObject sponza;
        if (!sponza.LoadModel(L"Models\\Sponza\\glTF\\Sponza.gltf", SModelObject::ModelFormat::Gltf, 0.01f)) {
            app.RequestQuit();
            return;
        }
        appendProxies(std::move(sponza));

        m_renderer->SubmitRenderProxies(std::move(rendererProxies));

        if (!ImGuiCoordinator::Instance().Initialize(app.GetHwnd(),
                                                 m_renderer->GetNativeDevice(),
                                                 m_renderer->GetNativeCommandQueue(),
                                                 m_renderer->GetBackBufferFormat(),
                                                 m_renderer->GetDepthFormat(),
                                                 static_cast<int>(m_renderer->GetBackBufferCount()))) {
            app.RequestQuit();
            return;
        }

        BindInputEvents();
        RegisterUi();
    }

    void RenderingApp::OnUpdate(Application& app, float deltaTime)
    {
        (void)app;
        m_camera.Update(deltaTime);
        if (m_renderer) {
            m_renderer->SetDeltaTime(deltaTime);
        }
    }

    void RenderingApp::OnRender(Application& app)
    {
        if (!m_renderer) return;
        const RenderCameraProxy cameraProxy =
            m_camera.BuildRenderCameraProxy(static_cast<float>(app.GetWidth()), static_cast<float>(app.GetHeight()));
        m_renderer->UpdateCameraCB(&cameraProxy);
        m_renderer->Render([](CommandList& cmdList, CpuDescriptorHandle rtvHandle) {
            ImGuiCoordinator::Instance().Render(cmdList.Get(), rtvHandle);
        });
    }

    void RenderingApp::OnShutdown(Application& app)
    {
        (void)app;
        ImGuiCoordinator::Instance().Shutdown();
        m_inputConnections.clear();
        m_renderer.reset();
    }

    void RenderingApp::OnResize(Application& app, UINT width, UINT height)
    {
        (void)app;
        if (m_renderer) {
            m_renderer->ResizeViewport(width, height);
        }
    }

    void RenderingApp::BindInputEvents()
    {
        auto& input = InputSystem::Instance();
        m_inputConnections.emplace_back(input.ConnectOnKeyDown([this](WPARAM k) { m_camera.OnKeyDown(k); }));
        m_inputConnections.emplace_back(input.ConnectOnKeyUp([this](WPARAM k) { m_camera.OnKeyUp(k); }));
        m_inputConnections.emplace_back(input.ConnectOnMouseDown([this](int x, int y) { m_camera.OnMouseDown(x, y); }));
        m_inputConnections.emplace_back(input.ConnectOnMouseUp([this]() { m_camera.OnMouseUp(); }));
        m_inputConnections.emplace_back(input.ConnectOnMouseMove([this](int x, int y, bool held) { m_camera.OnMouseMove(x, y, held); }));
        m_inputConnections.emplace_back(input.ConnectOnMouseWheel([this](int d) { m_camera.OnMouseWheel(d); }));
    }

    void RenderingApp::RegisterUi()
    {
        ImGuiCoordinator::Instance().RegisterWindow("Camera", [this]() {
            float speed = m_camera.MoveSpeed();
            if (ImGui::SliderFloat("Move Speed", &speed, 0.01f, 20.0f)) {
                m_camera.SetMoveSpeed(speed);
            }

            float nearClip = m_camera.NearClip();
            float farClip = m_camera.FarClip();
            bool clipChanged = false;
            clipChanged |= ImGui::SliderFloat("Near Clip", &nearClip, 0.0001f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
            clipChanged |= ImGui::SliderFloat("Far Clip", &farClip, 1.0f, 5000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            if (clipChanged) {
                m_camera.SetClipPlanes(nearClip, farClip);
            }
        });

        ImGuiCoordinator::Instance().RegisterWindow("Lighting", [this]() {
            if (!m_renderer) return;

            auto settings = m_renderer->GetDirectionalLightSettings();
            bool changed = false;
            changed |= ImGui::SliderAngle("Yaw", &settings.yaw, -180.0f, 180.0f);
            changed |= ImGui::SliderAngle("Pitch", &settings.pitch, -89.0f, 89.0f);
            changed |= ImGui::SliderFloat("Distance", &settings.distance, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("Ortho Half", &settings.orthoHalf, 0.1f, 10.0f);
            changed |= ImGui::SliderFloat("Near", &settings.nearZ, 0.01f, 100.0f);
            changed |= ImGui::SliderFloat("Far", &settings.farZ, 0.02f, 100.0f);
            changed |= ImGui::ColorEdit3("Dir Color", settings.color);
            changed |= ImGui::SliderFloat("Dir Intensity", &settings.intensity, 0.0f, 10.0f);

            if (settings.nearZ < 0.001f) {
                settings.nearZ = 0.001f;
                changed = true;
            }
            if (settings.farZ <= settings.nearZ + 0.01f) {
                settings.farZ = settings.nearZ + 0.02f;
                changed = true;
            }
            if (changed) {
                m_renderer->SetDirectionalLightSettings(settings);
            }

            if (m_renderer->GetDeltaTime() > 0.0f) {
                ImGui::Text("FPS: %.1f", 1.0f / m_renderer->GetDeltaTime());
            } else {
                ImGui::Text("FPS: --");
            }

            ImGui::Separator();
            auto& pointLights = m_renderer->GetPointLights();
            ImGui::Text("Point Lights: %d", static_cast<int>(pointLights.size()));
            if (ImGui::Button("Add Point")) { pointLights.push_back(Renderer::PointLight{}); }
            ImGui::SameLine();
            if (ImGui::Button("Remove Point") && !pointLights.empty()) { pointLights.pop_back(); }
            for (size_t i = 0; i < pointLights.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("Point %d", static_cast<int>(i));
                auto& pl = pointLights[i];
                ImGui::DragFloat3("Pos", pl.pos, 0.05f);
                ImGui::ColorEdit3("Color", pl.color);
                ImGui::SliderFloat("Intensity", &pl.intensity, 0.0f, 10.0f);
                ImGui::SliderFloat("Range", &pl.range, 0.1f, 50.0f);
                if (pl.range < 0.01f) pl.range = 0.01f;
                ImGui::PopID();
            }

            ImGui::Separator();
            auto& spotLights = m_renderer->GetSpotLights();
            ImGui::Text("Spot Lights: %d", static_cast<int>(spotLights.size()));
            if (ImGui::Button("Add Spot")) { spotLights.push_back(Renderer::SpotLight{}); }
            ImGui::SameLine();
            if (ImGui::Button("Remove Spot") && !spotLights.empty()) { spotLights.pop_back(); }
            for (size_t i = 0; i < spotLights.size(); ++i) {
                ImGui::PushID(100 + static_cast<int>(i));
                ImGui::Text("Spot %d", static_cast<int>(i));
                auto& sl = spotLights[i];
                ImGui::DragFloat3("Pos", sl.pos, 0.05f);
                ImGui::SliderAngle("Yaw", &sl.yaw, -180.0f, 180.0f);
                ImGui::SliderAngle("Pitch", &sl.pitch, -89.0f, 89.0f);
                ImGui::ColorEdit3("Color", sl.color);
                ImGui::SliderFloat("Intensity", &sl.intensity, 0.0f, 10.0f);
                ImGui::SliderFloat("Range", &sl.range, 0.1f, 50.0f);
                ImGui::SliderAngle("Inner", &sl.innerAngle, 0.0f, 89.0f);
                ImGui::SliderAngle("Outer", &sl.outerAngle, 0.0f, 89.0f);
                if (sl.range < 0.01f) sl.range = 0.01f;
                if (sl.innerAngle > sl.outerAngle) sl.innerAngle = sl.outerAngle;
                ImGui::PopID();
            }

            ImGui::Separator();
            bool useTessellation = m_renderer->GetUseTessellation();
            if (ImGui::Checkbox("Use Tessellation + GS", &useTessellation)) {
                m_renderer->SetUseTessellation(useTessellation);
            }
        });
    }
}
