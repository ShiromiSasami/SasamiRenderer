#include "ToonApp.h"
#include "ApplicationCore.h"
#include "Light/DefaultSunLight.h"
#include "ApplicationEntryPoint.h"
#include "AppFramework/Debug/ParticleCharacterOverlay.h"
#include "Input/CameraInputBinding.h"
#include "Input/InputSystem.h"
#include "Object/StaticModel.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UIVolume.h"
#include "UI/UIColor.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Toon
{
    namespace
    {
        // Every surface in this sample opts into ShadingModel::Toon; that is the whole point of
        // the sample. Roughness/metallic are still set because the GBuffer carries them and the
        // scene should stay meaningful if someone flips a material back to Standard to compare.
        SurfaceMaterial MakeToonMaterial(float r, float g, float b)
        {
            SurfaceMaterial material{};
            material.baseColor[0] = r;
            material.baseColor[1] = g;
            material.baseColor[2] = b;
            material.baseColor[3] = 1.0f;
            material.roughness = 0.6f;
            material.metallic = 0.0f;
            material.reflectionStrength = 0.0f; // cel surfaces should not pick up reflections
            material.occlusionStrength = 1.0f;
            material.shadingModel = ShadingModel::Toon;
            return material;
        }
    }

    void ToonApp::OnInit(ApplicationCore& app)
    {
        m_app = &app;
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("ToonApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("ToonApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        m_camera->SetTarget(m_camX, m_camY, m_camZ);
        m_camera->SetYawPitch(m_yaw, m_pitch);
        m_camera->SetMoveSpeed(4.0f);
        m_sceneGeneration = app.GetSceneGeneration();

        app.UseToonRenderNodePreset();

        StaticModel* floorModel = app.CreateStaticModel();
        if (!floorModel) {
            DebugLog("ToonApp: Floor model creation failed.\n");
            app.RequestQuit();
            return;
        }
        StaticModel::BoxDesc floorDesc{};
        floorDesc.width = 12.0f;
        floorDesc.height = 0.2f;
        floorDesc.depth = 12.0f;
        floorDesc.material = MakeToonMaterial(0.62f, 0.66f, 0.72f);
        floorModel->AddBox(floorDesc);
        floorModel->SetTranslation(0.0f, -0.1f, 0.5f);

        StaticModel* boxModel = app.CreateStaticModel();
        if (!boxModel) {
            DebugLog("ToonApp: Box model creation failed.\n");
            app.RequestQuit();
            return;
        }
        StaticModel::BoxDesc boxDesc{};
        boxDesc.width = 1.2f;
        boxDesc.height = 1.2f;
        boxDesc.depth = 1.2f;
        boxDesc.material = MakeToonMaterial(0.92f, 0.42f, 0.48f);
        boxModel->AddBox(boxDesc);
        boxModel->SetTranslation(-1.6f, 0.6f, 0.5f);

        // Spheres carry the sample: a box only ever shows one flat band per face, so the
        // quantized terminator and the rim are only legible on curved geometry.
        struct SpherePlacement { float x, y, z, radius, r, g, b; };
        static const SpherePlacement kSpheres[] = {
            {  0.6f, 0.85f,  0.2f, 0.85f, 0.35f, 0.62f, 0.92f },
            {  2.4f, 0.55f, -0.9f, 0.55f, 0.98f, 0.82f, 0.35f },
            { -0.4f, 0.40f, -2.0f, 0.40f, 0.55f, 0.88f, 0.60f },
        };
        for (const SpherePlacement& p : kSpheres) {
            StaticModel* sphere = app.CreateStaticModel();
            if (!sphere) {
                DebugLog("ToonApp: Sphere model creation failed.\n");
                continue;
            }
            StaticModel::SphereDesc sphereDesc{};
            sphereDesc.radius = p.radius;
            sphereDesc.material = MakeToonMaterial(p.r, p.g, p.b);
            sphere->AddSphere(sphereDesc);
            sphere->SetTranslation(p.x, p.y, p.z);
        }

        // Set up directional light (sun)
        // Much lower than a PBR scene would use. Measured at 1.1: every toon surface landed
        // at 195-216/255, inside the tonemapper's shoulder, where its highlight desaturation
        // pulled the base colours to neutral grey and squashed the lit and shadow bands to
        // within 2/255 of each other. The banding only reads once the mid tones sit lower.
        app.SetDirectionalLight(MakeDefaultSunLight(0.5f));

        // Input bindings
        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });

        // UI window
        ImGuiCoordinator::Instance().RegisterWindow("Toon Shading", [this]() {
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
            ImGui::Text("Cel Shading");
            ImGui::TextDisabled("Global: applies to every ShadingModel::Toon surface.");

            Renderer& renderer = m_app->GetRenderer();
            LightSystem::ToonSettings toon = renderer.GetToonSettings();
            bool changed = false;
            changed |= UI::Volume("Bands", &toon.bandCount, 1.0f, 8.0f, "%.0f");
            changed |= UI::Volume("Band Softness", &toon.bandSoftness, 0.0f, 0.25f);
            changed |= UI::Volume("Specular Threshold", &toon.specularThreshold, 0.0f, 1.0f);
            changed |= UI::Volume("Specular Softness", &toon.specularSoftness, 0.0f, 0.2f);
            changed |= UI::Volume("Specular Intensity", &toon.specularIntensity, 0.0f, 2.0f);
            changed |= UI::Volume("Rim Strength", &toon.rimStrength, 0.0f, 1.5f);
            changed |= UI::Volume("Rim Power", &toon.rimPower, 0.5f, 8.0f);
            changed |= UI::Volume("Shadow Floor", &toon.shadowFloor, 0.0f, 0.8f);
            changed |= UI::Volume("Ambient Scale", &toon.ambientScale, 0.0f, 1.0f);
            changed |= UI::Volume("Outline Width", &toon.outlineWidth, 0.0f, 0.1f, "%.4f");
            changed |= UI::Color3("Outline Color", toon.outlineColor);
            changed |= UI::Volume("Depth Rim Width (px@1m)", &toon.depthRimWidth, 0.0f, 40.0f, "%.1f");
            changed |= UI::Volume("Depth Rim Threshold", &toon.depthRimThreshold, 0.01f, 2.0f);
            changed |= UI::Volume("Depth Rim Strength", &toon.depthRimStrength, 0.0f, 2.0f);
            if (changed) {
                renderer.SetToonSettings(toon);
            }

            ImGui::Separator();
            ImGui::Text("Particle Character");
            ImGui::TextDisabled("Fox surface as molecular particles (CPU, no depth test). Same as pchar.view.");
            Debug::ParticleCharacterOverlay& pchar = m_app->GetParticleCharacterOverlay();
            static int pcharCount = 8000;
            static float pcharCycle = 0.0f;
            ImGui::SliderInt("Particles", &pcharCount, 1000, 50000);
            if (!pchar.IsRunning()) {
                if (ImGui::Button("Show Particle Fox")) {
                    Debug::ParticleCharacterViewSettings view;
                    view.count = static_cast<uint32_t>(pcharCount);
                    view.cycle = pcharCycle;
                    DebugLog(("[ToonApp] pchar.view " + pchar.Start(view) + "\n").c_str());
                }
            } else {
                if (ImGui::Button("Hide")) {
                    pchar.Stop();
                }
                ImGui::SameLine();
                if (ImGui::Button(pchar.BondGoal() < 0.5f ? "Re-form" : "Release")) {
                    pchar.SetBondGoal(pchar.BondGoal() < 0.5f ? 1.0f : 0.0f);
                }
                int force = static_cast<int>(pchar.Force());
                if (ImGui::Combo("Release Mode", &force, "Scatter\0Swirl\0Melt\0")) {
                    pchar.SetForce(static_cast<Render::ParticleCharacter::MolecularForce>(force));
                }
            }
            if (UI::Volume("Auto Cycle (s, 0=off)", &pcharCycle, 0.0f, 8.0f) && pchar.IsRunning()) {
                pchar.SetCycle(pcharCycle);
            }
        });
    }

    void ToonApp::RefreshCameraAfterSceneLoad(ApplicationCore& app)
    {
        if (app.GetSceneGeneration() == m_sceneGeneration) {
            return;
        }
        m_sceneGeneration = app.GetSceneGeneration();
        m_camera = app.GetMainCamera();
        DebugLog("[ToonApp] scene replaced; camera pointer refreshed\n");
    }

    void ToonApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        RefreshCameraAfterSceneLoad(app);
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void ToonApp::OnRender(ApplicationCore& app)
    {
        // Debug-pipe commands drain after OnUpdate, and the UI reads m_camera.
        RefreshCameraAfterSceneLoad(app);
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void ToonApp::OnShutdown(ApplicationCore& app)
    {
        m_inputConnections.clear();
        app.ClearObjects();
        m_camera = nullptr;
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::Toon::ToonApp, 1280, 720, L"Toon Shading Sample")
