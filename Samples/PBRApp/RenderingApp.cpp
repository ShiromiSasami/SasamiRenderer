#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "Object/StaticModel.h"
#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"
#include "ApplicationEntryPoint.h"
#include "Foundation/Tools/DebugOutput.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace SasamiRenderer
{
    namespace
    {
        constexpr const char* kSkyboxPanoramaPath = "Assets/HDR/citrus_orchard_road_puresky_4k.hdr";
        constexpr const char* kStanfordBunnyPath = "Models/stanford_bunny_pbr/scene.gltf";
        constexpr const char* kSponzaPath = "Models/Sponza/glTF/Sponza.gltf";
    }

    void RenderingApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        app.SetRenderNodeSequence({
            ApplicationCore::RenderNodeType::Shadow,
            ApplicationCore::RenderNodeType::Opaque,
            ApplicationCore::RenderNodeType::Lighting,
            ApplicationCore::RenderNodeType::Skybox,
            ApplicationCore::RenderNodeType::Transparent,
            ApplicationCore::RenderNodeType::TransparentLighting,
            ApplicationCore::RenderNodeType::PostProcess,
        });

        if (!app.LoadSkybox(kSkyboxPanoramaPath, ApplicationCore::SkyboxLoadFormat::HdrEquirect)) {
            DebugLog("Skybox load failed: invalid path for selected format.\n");
        }

        StaticModel* bunnyModel = app.CreateStaticModel();
        if (!bunnyModel || !bunnyModel->LoadModel(kStanfordBunnyPath, StaticModel::ModelFormat::Gltf, 0.0001f)) {
            if (bunnyModel) {
                app.DeleteObject(bunnyModel);
                bunnyModel = nullptr;
            }
            DebugLog("Bunny model load failed: Assets/Models/stanford_bunny_pbr/scene.gltf\n");
        }

        StaticModel* sponzaModel = app.CreateStaticModel();
        if (!sponzaModel || !sponzaModel->LoadModel(kSponzaPath, StaticModel::ModelFormat::Gltf, 0.01f)) {
            if (sponzaModel) {
                app.DeleteObject(sponzaModel);
                sponzaModel = nullptr;
            }
            DebugLog("Sponza model load failed: Assets/Models/Sponza/glTF/Sponza.gltf\n");
        }

        if (!bunnyModel && !sponzaModel) {
            DebugLog("No models were created. Application will quit.\n");
            app.RequestQuit();
            return;
        }

        BindInputEvents(app);
        RegisterUi(app);
    }

    void RenderingApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        (void)app;
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void RenderingApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void RenderingApp::OnShutdown(ApplicationCore& app)
    {
        app.ClearObjects();
        m_camera = nullptr;
        m_inputConnections.clear();
    }

    void RenderingApp::OnResize(ApplicationCore& app, UINT width, UINT height)
    {
        (void)app;
        (void)width;
        (void)height;
    }

    void RenderingApp::BindInputEvents(ApplicationCore& app)
    {
        auto& input = InputSystem::Instance();
        m_inputConnections.emplace_back(input.ConnectOnKeyDown([this](WPARAM k) {
            if (m_camera) {
                m_camera->OnKeyDown(k);
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnKeyUp([this](WPARAM k) {
            if (m_camera) {
                m_camera->OnKeyUp(k);
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnKeyUp([&app](WPARAM k) {
            if (k == VK_F2) {
                app.CycleGBufferDebugView(1);
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnMouseDown([this](int x, int y) {
            if (m_camera) {
                m_camera->OnMouseDown(x, y);
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnMouseUp([this]() {
            if (m_camera) {
                m_camera->OnMouseUp();
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnMouseMove([this](int x, int y, bool held) {
            if (m_camera) {
                m_camera->OnMouseMove(x, y, held);
            }
        }));
        m_inputConnections.emplace_back(input.ConnectOnMouseWheel([this](int d) {
            if (m_camera) {
                m_camera->OnMouseWheel(d);
            }
        }));
    }

    void RenderingApp::RegisterUi(ApplicationCore& app)
    {
        ImGuiCoordinator::Instance().RegisterWindow("Camera", [this, &app]() {
            if (!m_camera) {
                ImGui::TextDisabled("Camera object is not available.");
                return;
            }

            float speed = m_camera->MoveSpeed();
            if (ImGui::SliderFloat("Move Speed", &speed, 0.01f, 20.0f)) {
                m_camera->SetMoveSpeed(speed);
            }

            float nearClip = m_camera->NearClip();
            float farClip = m_camera->FarClip();
            bool clipChanged = false;
            clipChanged |= ImGui::SliderFloat("Near Clip", &nearClip, 0.0001f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
            clipChanged |= ImGui::SliderFloat("Far Clip", &farClip, 1.0f, 5000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            if (clipChanged) {
                m_camera->SetClipPlanes(nearClip, farClip);
            }

            const auto& transform = m_camera->Transform();
            ImGui::Text("Position: (%.3f, %.3f, %.3f)",
                        transform.position.x,
                        transform.position.y,
                        transform.position.z);

            const auto& proxy = app.GetMainCameraProxy();
            ImGui::Text("Proxy Pos: (%.3f, %.3f, %.3f)",
                        proxy.cameraPosition[0],
                        proxy.cameraPosition[1],
                        proxy.cameraPosition[2]);
        });

        ImGuiCoordinator::Instance().RegisterWindow("Lighting", [this, &app]() {
            auto light = app.GetDirectionalLight();
            bool changed = false;
            changed |= ImGui::SliderAngle("Yaw", &light.yaw, -180.0f, 180.0f);
            changed |= ImGui::SliderAngle("Pitch", &light.pitch, -89.0f, 89.0f);
            changed |= ImGui::SliderFloat("Distance", &light.distance, 0.1f, 20.0f);
            changed |= ImGui::SliderFloat("Ortho Half", &light.orthoHalf, 0.1f, 10.0f);
            changed |= ImGui::SliderFloat("Near", &light.nearZ, 0.01f, 100.0f);
            changed |= ImGui::SliderFloat("Far", &light.farZ, 0.02f, 100.0f);
            changed |= ImGui::ColorEdit4("Dir Color", light.color.Data());
            changed |= ImGui::SliderFloat("Dir Intensity", &light.intensity, 0.0f, 10.0f);

            if (light.nearZ < 0.001f) {
                light.nearZ = 0.001f;
                changed = true;
            }
            if (light.farZ <= light.nearZ + 0.01f) {
                light.farZ = light.nearZ + 0.02f;
                changed = true;
            }
            if (changed) {
                app.SetDirectionalLight(light);
            }

            if (app.GetDeltaTime() > 0.0f) {
                ImGui::Text("FPS: %.1f", 1.0f / app.GetDeltaTime());
            } else {
                ImGui::Text("FPS: --");
            }

            ImGui::Separator();
            auto pointLights = app.GetPointLightObjects();
            ImGui::Text("Point Lights: %d", static_cast<int>(pointLights.size()));
            if (ImGui::Button("Add Point")) {
                app.CreatePointLightObject();
                pointLights = app.GetPointLightObjects();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Point") && !pointLights.empty()) {
                app.DeleteObject(pointLights.back());
                pointLights = app.GetPointLightObjects();
            }
            for (size_t i = 0; i < pointLights.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("Point %d", static_cast<int>(i));
                auto* pl = pointLights[i];
                if (!pl) {
                    ImGui::PopID();
                    continue;
                }
                auto& tr = pl->Transform();
                ImGui::DragFloat3("Pos", &tr.position.x, 0.05f);
                ImGui::ColorEdit4("Color", pl->ColorData());
                float intensity = pl->Intensity();
                if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 10.0f)) {
                    pl->SetIntensity(intensity);
                }
                float range = pl->Range();
                if (ImGui::SliderFloat("Range", &range, 0.1f, 50.0f)) {
                    if (range < 0.01f) {
                        range = 0.01f;
                    }
                    pl->SetRange(range);
                }
                ImGui::PopID();
            }

            ImGui::Separator();
            auto spotLights = app.GetSpotLightObjects();
            ImGui::Text("Spot Lights: %d", static_cast<int>(spotLights.size()));
            if (ImGui::Button("Add Spot")) {
                app.CreateSpotLightObject();
                spotLights = app.GetSpotLightObjects();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Spot") && !spotLights.empty()) {
                app.DeleteObject(spotLights.back());
                spotLights = app.GetSpotLightObjects();
            }
            for (size_t i = 0; i < spotLights.size(); ++i) {
                ImGui::PushID(100 + static_cast<int>(i));
                ImGui::Text("Spot %d", static_cast<int>(i));
                auto* sl = spotLights[i];
                if (!sl) {
                    ImGui::PopID();
                    continue;
                }
                auto& tr = sl->Transform();
                ImGui::DragFloat3("Pos", &tr.position.x, 0.05f);
                ImGui::SliderAngle("Yaw", &tr.rotation.yaw, -180.0f, 180.0f);
                ImGui::SliderAngle("Pitch", &tr.rotation.pitch, -89.0f, 89.0f);
                ImGui::ColorEdit4("Color", sl->ColorData());

                float intensity = sl->Intensity();
                if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 10.0f)) {
                    sl->SetIntensity(intensity);
                }
                float range = sl->Range();
                if (ImGui::SliderFloat("Range", &range, 0.1f, 50.0f)) {
                    if (range < 0.01f) {
                        range = 0.01f;
                    }
                    sl->SetRange(range);
                }

                float innerAngle = sl->InnerAngle();
                float outerAngle = sl->OuterAngle();
                bool angleChanged = false;
                angleChanged |= ImGui::SliderAngle("Inner", &innerAngle, 0.0f, 89.0f);
                angleChanged |= ImGui::SliderAngle("Outer", &outerAngle, 0.0f, 89.0f);
                if (innerAngle > outerAngle) {
                    innerAngle = outerAngle;
                    angleChanged = true;
                }
                if (angleChanged) {
                    sl->SetInnerAngle(innerAngle);
                    sl->SetOuterAngle(outerAngle);
                }
                ImGui::PopID();
            }

            ImGui::Separator();
            bool useTessellation = app.GetUseTessellation();
            if (ImGui::Checkbox("Use Tessellation + GS", &useTessellation)) {
                app.SetUseTessellation(useTessellation);
            }

            int rasterMode = app.GetRasterShaderModeIndex();
            if (ImGui::Combo("Raster Shader", &rasterMode, "Lighting (PBR)\0Opaque (Unlit)\0")) {
                app.SetRasterShaderModeIndex(rasterMode);
            }

            int debugMode = app.GetGBufferDebugViewIndex();
            if (ImGui::Combo("GBuffer Debug", &debugMode,
                             "Final Lit\0Albedo\0Normal\0Roughness\0Metallic\0Ambient Occlusion\0Shadow\0")) {
                app.SetGBufferDebugViewIndex(debugMode);
            }
            ImGui::TextDisabled("Shortcut: F2 (cycle)");

            ImGui::TextDisabled("Mesh Shader Path: TODO (planned comparison target)");
        });
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiRenderer::RenderingApp, 1280, 720, L"PBR App")
