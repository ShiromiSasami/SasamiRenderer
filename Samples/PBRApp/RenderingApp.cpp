#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "Object/StaticModel.h"
#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"
#include "ApplicationEntryPoint.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Structures/RendererEnums.h"
#include "imgui.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        constexpr const char* kSkyboxPanoramaPath = "Assets/HDR/citrus_orchard_road_puresky_4k.hdr";
        constexpr const char* kStanfordBunnyPath = "Models/stanford_bunny_pbr/scene.gltf";

        float DefaultReflectionStrength(float roughness, float metallic)
        {
            const float smoothness = 1.0f - std::fmin(std::fmax(roughness, 0.0f), 1.0f);
            const float strength = std::fmin(std::fmax(metallic, 0.0f), 1.0f) * smoothness;
            return std::fmin(std::fmax(strength, 0.0f), 1.0f);
        }
        constexpr const char* kSponzaPath = "Models/Sponza/glTF/Sponza.gltf";

        struct RenderPassBuilderUiEntry
        {
            const char* label = "";
            RendererEnums::RenderPassType type = RendererEnums::RenderPassType::Opaque;
        };

        constexpr std::array<RenderPassBuilderUiEntry, 15> kRenderPassBuilderUiEntries = {{
            { "Shadow", RendererEnums::RenderPassType::Shadow },
            { "Opaque", RendererEnums::RenderPassType::Opaque },
            { "Runtime AO", RendererEnums::RenderPassType::RuntimeAO },
            { "Runtime AO Blur", RendererEnums::RenderPassType::RuntimeAOBlur },
            { "Lighting", RendererEnums::RenderPassType::Lighting },
            { "Software Reflection", RendererEnums::RenderPassType::SoftwareReflection },
            { "Software Reflection Composite", RendererEnums::RenderPassType::SoftwareReflectionComposite },
            { "Skybox", RendererEnums::RenderPassType::Skybox },
            { "Procedural Sky", RendererEnums::RenderPassType::ProceduralSky },
            { "Transparent Backface Distance", RendererEnums::RenderPassType::TransparentBackfaceDistance },
            { "Transparent Scene Color Copy", RendererEnums::RenderPassType::TransparentSceneColorCopy },
            { "Transparent", RendererEnums::RenderPassType::Transparent },
            { "Transparent Lighting", RendererEnums::RenderPassType::TransparentLighting },
            { "Transparent Composite", RendererEnums::RenderPassType::TransparentComposite },
            { "Post Process", RendererEnums::RenderPassType::PostProcess },
        }};

        bool HasRenderPass(const std::vector<ApplicationCore::RenderPassType>& sequence,
                           RendererEnums::RenderPassType type)
        {
            return std::find(sequence.begin(), sequence.end(), type) != sequence.end();
        }

        void DrawRenderPassBuilderControls(ApplicationCore& app)
        {
            if (!ImGui::CollapsingHeader("Render Node Preset", ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }

            std::vector<ApplicationCore::RenderPassType> currentSequence = app.GetRenderPassSequence();
            std::vector<ApplicationCore::RenderPassType> nextSequence;
            nextSequence.reserve(kRenderPassBuilderUiEntries.size());
            bool changed = false;

            for (const RenderPassBuilderUiEntry& entry : kRenderPassBuilderUiEntries) {
                bool enabled = HasRenderPass(currentSequence, entry.type);
                if (ImGui::Checkbox(entry.label, &enabled)) {
                    changed = true;
                }
                if (enabled) {
                    nextSequence.push_back(entry.type);
                }
            }

            if (ImGui::Button("Reset Passes")) {
                app.UseDefaultRenderNodePreset();
                return;
            }

            if (changed && !nextSequence.empty()) {
                app.SetRenderPassSequence(nextSequence);
            }
        }

        void ApplyProbeGridPreset(ApplicationCore& app, int presetIndex)
        {
            switch (presetIndex) {
            case 0:
                // Sponza interior coverage.
                app.FitProbeGridToScene(-7.0f, -0.5f, -13.0f, 7.0f, 8.0f, 15.0f, 1.0f);
                break;
            case 2:
                // Wide interior plus large exterior buffer and more sky coverage.
                app.FitProbeGridToScene(-18.0f, -2.0f, -24.0f, 18.0f, 18.0f, 26.0f, 3.0f);
                break;
            case 1:
            default:
                // Default: wider than the old Sponza fit so probe debug is easier to inspect.
                app.FitProbeGridToScene(-12.0f, -1.0f, -18.0f, 12.0f, 12.0f, 20.0f, 2.0f);
                break;
            }
        }

        SurfaceMaterial MakeMaterial(float r, float g, float b, float roughness, float metallic,
                                     float emissiveR = 0.0f, float emissiveG = 0.0f, float emissiveB = 0.0f)
        {
            SurfaceMaterial material{};
            material.baseColor[0] = r;
            material.baseColor[1] = g;
            material.baseColor[2] = b;
            material.baseColor[3] = 1.0f;
            material.emissive[0] = emissiveR;
            material.emissive[1] = emissiveG;
            material.emissive[2] = emissiveB;
            material.roughness = roughness;
            material.metallic = metallic;
            material.reflectionStrength = DefaultReflectionStrength(roughness, metallic);
            material.occlusionStrength = 1.0f;
            return material;
        }

        SurfaceMaterial MakeTransparentMaterial(float r, float g, float b, float alpha,
                                                float roughness, float metallic,
                                                float transmission, float ior)
        {
            SurfaceMaterial material = MakeMaterial(r, g, b, roughness, metallic);
            material.baseColor[3] = alpha;
            material.transmission = transmission;
            material.ior = ior;
            return material;
        }

        bool DrawMaterialEditor(const char* label, SurfaceMaterial& material)
        {
            bool changed = false;
            ImGui::PushID(label);
            ImGui::TextDisabled("%s", label);
            ImGui::Separator();
            int workflowIndex = static_cast<int>(material.workflow);
            if (ImGui::Combo("Workflow", &workflowIndex, "Metallic-Roughness\0Specular-Glossiness\0")) {
                if (workflowIndex < 0) {
                    workflowIndex = 0;
                } else if (workflowIndex > 1) {
                    workflowIndex = 1;
                }
                material.workflow = static_cast<MaterialWorkflow>(workflowIndex);
                changed = true;
            }

            if (material.workflow == MaterialWorkflow::SpecularGlossiness) {
                changed |= ImGui::ColorEdit4("Diffuse Color", material.baseColor);
                changed |= ImGui::ColorEdit3("Specular Color", material.specularColor);
                float glossiness = 1.0f - material.roughness;
                if (ImGui::SliderFloat("Glossiness", &glossiness, 0.0f, 1.0f)) {
                    material.roughness = 1.0f - glossiness;
                    changed = true;
                }
            } else {
                changed |= ImGui::ColorEdit4("Base Color", material.baseColor);
                changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f);
            }
            changed |= ImGui::ColorEdit3("Emissive", material.emissive);
            changed |= ImGui::SliderFloat("Reflection Strength", &material.reflectionStrength, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("AO Strength", &material.occlusionStrength, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Transmission", &material.transmission, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("IOR", &material.ior, 1.0f, 2.5f);
            if (material.baseColor[3] < 0.999f || material.transmission > 0.0f) {
                changed |= ImGui::SliderFloat("Transparent Shell", &material.transparentShellStrength, 0.0f, 2.0f);
            }
            ImGui::PopID();
            return changed;
        }
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
        m_camera->SetTarget(0.0f, 1.5f, -7.5f);
        m_camera->SetYawPitch(0.0f, 0.12f);
        m_camera->SetMoveSpeed(4.0f);

        const bool useFullDx12RenderGraph = (app.GetGraphicsRuntime() == GraphicsRuntime::DirectX12);
        if (useFullDx12RenderGraph) {
            app.UseDefaultRenderNodePreset();

            if (!app.LoadSkybox(kSkyboxPanoramaPath, ApplicationCore::SkyboxLoadFormat::HdrEquirect)) {
                DebugLog("Skybox load failed: invalid path for selected format.\n");
            }
        } else {
            DebugLog("PBRApp: non-DX12 backend uses native mesh frame path; DX12 render graph skybox/shadow/post passes are skipped.\n");
        }

        m_sphereMaterial = MakeMaterial(0.98f, 0.84f, 0.32f, 0.05f, 1.0f);
        m_boxMaterial = MakeMaterial(0.72f, 0.80f, 0.90f, 0.14f, 1.0f);
        m_transparentSphereMaterial = MakeTransparentMaterial(0.35f, 0.78f, 0.95f, 0.35f, 0.04f, 0.0f, 0.92f, 1.33f);
        m_transparentBoxMaterial = MakeTransparentMaterial(0.92f, 0.58f, 0.26f, 0.42f, 0.08f, 0.0f, 0.78f, 1.50f);

        StaticModel* bunnyModel = app.CreateStaticModel();
        StaticModel* sponzaModel = app.CreateStaticModel();
        m_sphereModel = app.CreateStaticModel();
        m_boxModel = app.CreateStaticModel();
        m_transparentSphereModel = app.CreateStaticModel();
        m_transparentBoxModel = app.CreateStaticModel();
        StaticModel* floorModel = app.CreateStaticModel();

        if (!bunnyModel || !sponzaModel || !m_sphereModel || !m_boxModel ||
            !m_transparentSphereModel || !m_transparentBoxModel || !floorModel) {
            DebugLog("Sample scene object creation failed.\n");
            app.RequestQuit();
            return;
        }

        if (!bunnyModel->LoadModel(kStanfordBunnyPath, StaticModel::ModelFormat::Gltf, 0.01f)) {
            app.DeleteObject(bunnyModel);
            bunnyModel = nullptr;
            DebugLog("Bunny model load failed: Assets/Models/stanford_bunny_pbr/scene.gltf\n");
        } else {
            bunnyModel->SetTranslation(0.0f, 0.0f, 0.8f);
        }

        if (!sponzaModel->LoadModel(kSponzaPath, StaticModel::ModelFormat::Gltf, 1.f)) {
            app.DeleteObject(sponzaModel);
            sponzaModel = nullptr;
            DebugLog("Sponza model load failed: Assets/Models/Sponza/glTF/Sponza.gltf\n");
        } else {
            sponzaModel->SetTranslation(0.f, 0.f, 1.f);
        }

        StaticModel::SphereDesc sphereDesc{};
        sphereDesc.radius = 0.72f;
        sphereDesc.slices = 48u;
        sphereDesc.stacks = 24u;
        sphereDesc.material = m_sphereMaterial;
        m_sphereModel->AddSphere(sphereDesc);
        m_sphereModel->SetTranslation(-1.8f, 0.72f, 1.0f);

        StaticModel::BoxDesc boxDesc{};
        boxDesc.width = 1.18f;
        boxDesc.height = 1.18f;
        boxDesc.depth = 1.18f;
        boxDesc.material = m_boxMaterial;
        m_boxModel->AddBox(boxDesc);
        m_boxModel->SetTranslation(1.9f, 0.59f, 0.35f);

        StaticModel::SphereDesc transparentSphereDesc{};
        transparentSphereDesc.radius = 0.72f;
        transparentSphereDesc.slices = 48u;
        transparentSphereDesc.stacks = 24u;
        transparentSphereDesc.material = m_transparentSphereMaterial;
        m_transparentSphereModel->AddSphere(transparentSphereDesc);
        m_transparentSphereModel->SetTranslation(-4.0f, 0.72f, 1.05f);

        StaticModel::BoxDesc transparentBoxDesc{};
        transparentBoxDesc.width = 1.18f;
        transparentBoxDesc.height = 1.18f;
        transparentBoxDesc.depth = 1.18f;
        transparentBoxDesc.material = m_transparentBoxMaterial;
        m_transparentBoxModel->AddBox(transparentBoxDesc);
        m_transparentBoxModel->SetTranslation(4.0f, 0.59f, 0.35f);

        if (!sponzaModel) {
            SurfaceMaterial floorMaterial = MakeMaterial(0.52f, 0.54f, 0.58f, 0.92f, 0.0f);
            StaticModel::BoxDesc floorDesc{};
            floorDesc.width = 12.0f;
            floorDesc.height = 0.2f;
            floorDesc.depth = 12.0f;
            floorDesc.material = floorMaterial;
            floorModel->AddBox(floorDesc);
            floorModel->SetTranslation(0.0f, -0.1f, 0.5f);
        } else {
            app.DeleteObject(floorModel);
            floorModel = nullptr;
        }

        ApplyProbeGridPreset(app, m_probeGridPreset);

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
        m_sphereModel = nullptr;
        m_boxModel = nullptr;
        m_transparentSphereModel = nullptr;
        m_transparentBoxModel = nullptr;
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
            if (k == VK_F2 &&
                app.GetRenderPathModeIndex() == static_cast<int>(RendererEnums::RenderPathMode::Raster)) {
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
        ImGuiCoordinator::Instance().RegisterWindow("PBR Controls", [this, &app]() {
            if (!ImGui::BeginTabBar("PBRControlTabs")) {
                return;
            }

            if (ImGui::BeginTabItem("Camera")) {
            if (!m_camera) {
                ImGui::TextDisabled("Camera object is not available.");
            } else {

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

            ImGui::Separator();
            const float yawDeg   = m_camera->Yaw()   * (180.0f / 3.14159265f);
            const float pitchDeg = m_camera->Pitch() * (180.0f / 3.14159265f);
            ImGui::Text("Yaw:   %.2f deg  (%.4f rad)", yawDeg,   m_camera->Yaw());
            ImGui::Text("Pitch: %.2f deg  (%.4f rad)", pitchDeg, m_camera->Pitch());
            ImGui::Text("Forward: (%.3f, %.3f, %.3f)",
                        proxy.cameraForward[0],
                        proxy.cameraForward[1],
                        proxy.cameraForward[2]);
            ImGui::Text("Right:   (%.3f, %.3f, %.3f)",
                        proxy.cameraRight[0],
                        proxy.cameraRight[1],
                        proxy.cameraRight[2]);
            ImGui::Text("Up:      (%.3f, %.3f, %.3f)",
                        proxy.cameraUp[0],
                        proxy.cameraUp[1],
                        proxy.cameraUp[2]);
            }
            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Lighting")) {
            auto light = app.GetDirectionalLight();
            bool changed = false;
            changed |= ImGui::SliderAngle("Yaw", &light.yaw, -180.0f, 180.0f);
            changed |= ImGui::SliderAngle("Pitch", &light.pitch, -180.0f, 180.0f);
            changed |= ImGui::SliderFloat("Distance", &light.distance, 0.1f, 50.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Shadow camera distance from the scene center.\nThis is not physical light falloff distance.");
            }
            changed |= ImGui::SliderFloat("Ortho Half", &light.orthoHalf, 0.1f, 50.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Half-size of the directional shadow orthographic projection.\nLarger values cover more area but reduce shadow texel density.");
            }
            changed |= ImGui::SliderFloat("Near", &light.nearZ, 0.01f, 100.0f);
            changed |= ImGui::SliderFloat("Far", &light.farZ, 0.02f, 300.0f);
            changed |= ImGui::ColorEdit4("Dir Color", light.color.Data());
            changed |= ImGui::SliderFloat("Dir Intensity", &light.intensity, 0.0f, 10.0f);
            int shadowMode = static_cast<int>(light.shadowMode);
            changed |= ImGui::RadioButton("Single Shadow", &shadowMode, static_cast<int>(DirectionalShadowMode::Single));
            changed |= ImGui::RadioButton("CSM4", &shadowMode, static_cast<int>(DirectionalShadowMode::Csm4));
            changed |= ImGui::RadioButton("VSM", &shadowMode, static_cast<int>(DirectionalShadowMode::Vsm));
            changed |= ImGui::RadioButton("VSM4", &shadowMode, static_cast<int>(DirectionalShadowMode::Vsm4));
            light.shadowMode = static_cast<DirectionalShadowMode>(shadowMode);
            if (shadowMode >= static_cast<int>(DirectionalShadowMode::Vsm)) {
                bool vsmBlur = app.GetVsmBlurEnabled();
                if (ImGui::Checkbox("VSM Blur", &vsmBlur)) {
                    app.SetVsmBlurEnabled(vsmBlur);
                }
            }
            changed |= ImGui::SliderFloat("Shadow Distance", &light.shadowDistance, 5.0f, 250.0f);
            changed |= ImGui::SliderFloat("Cascade Exponent", &light.cascadeDistributionExponent, 1.0f, 4.0f);
            changed |= ImGui::SliderFloat("Cascade Blend", &light.cascadeBlendFraction, 0.0f, 0.3f);
            changed |= ImGui::SliderFloat("Depth Bias", &light.depthBias, 0.0f, 4000.0f);
            changed |= ImGui::SliderFloat("Slope Bias", &light.slopeScaleBias, 0.0f, 8.0f);
            changed |= ImGui::SliderFloat("Normal Bias", &light.normalBias, 0.0f, 0.1f);
            changed |= ImGui::SliderFloat("Far Bias Scale", &light.farBiasScale, 1.0f, 4.0f);

            if (light.nearZ < 0.001f) {
                light.nearZ = 0.001f;
                changed = true;
            }
            if (light.farZ <= light.nearZ + 0.01f) {
                light.farZ = light.nearZ + 0.02f;
                changed = true;
            }
            if (light.shadowDistance < 1.0f) {
                light.shadowDistance = 1.0f;
                changed = true;
            }
            if (changed) {
                app.SetDirectionalLight(light);
            }

            ImGui::Separator();
            ImGui::Checkbox("Show Light Gizmo", &m_showLightGizmo);

            // 3D light position/direction gizmo drawn on the viewport
            if (m_showLightGizmo) {
                const auto& proxy = app.GetMainCameraProxy();
                const float* vp = proxy.viewProjection;
                const float sw = static_cast<float>(app.GetWidth());
                const float sh = static_cast<float>(app.GetHeight());

                // Light forward direction from yaw/pitch (same as DirectionFromYawPitch)
                const float cy = std::cos(light.yaw),  sy = std::sin(light.yaw);
                const float cp = std::cos(light.pitch), sp = std::sin(light.pitch);
                float fx = -sy, fy = sp * cy, fz = -cp * cy;
                const float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
                if (fl > 1e-6f) { fx /= fl; fy /= fl; fz /= fl; }

                // Light world position: place gizmo farther back so it stays
                // visible outside the scene bounds (2.5× the shadow camera distance).
                const float gizmoDist = light.distance * 2.5f;
                const float lx = -fx * gizmoDist;
                const float ly = -fy * gizmoDist;
                const float lz = -fz * gizmoDist;

                // Arrow tip: midpoint along the ray toward scene
                const float arrowLen = gizmoDist * 0.45f;
                const float ax = lx + fx * arrowLen;
                const float ay = ly + fy * arrowLen;
                const float az = lz + fz * arrowLen;

                // World → screen (row-vector convention: clip = worldPos * VP)
                auto w2s = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
                    const float cx2 = wx*vp[0] + wy*vp[4] + wz*vp[8]  + vp[12];
                    const float cy2 = wx*vp[1] + wy*vp[5] + wz*vp[9]  + vp[13];
                    const float cw  = wx*vp[3] + wy*vp[7] + wz*vp[11] + vp[15];
                    if (std::abs(cw) < 1e-7f || cw < 0.0f) return false;
                    const float ndcX = cx2 / cw;
                    const float ndcY = cy2 / cw;
                    out = { (ndcX + 1.f) * 0.5f * sw, (1.f - ndcY) * 0.5f * sh };
                    return ndcX >= -1.1f && ndcX <= 1.1f && ndcY >= -1.1f && ndcY <= 1.1f;
                };

                ImVec2 lightScreen{}, arrowTip{};
                const bool lightOk = w2s(lx, ly, lz, lightScreen);
                const bool arrowOk = w2s(ax, ay, az, arrowTip);

                ImDrawList* drawList = ImGui::GetForegroundDrawList();
                constexpr ImU32 kYellow   = IM_COL32(255, 220, 50, 230);
                constexpr ImU32 kYellowBg = IM_COL32(255, 180, 0,  160);
                constexpr float kRadius   = 13.f;

                if (lightOk) {
                    // Filled circle (sun body)
                    drawList->AddCircleFilled(lightScreen, kRadius, kYellowBg, 20);
                    drawList->AddCircle(lightScreen, kRadius, kYellow, 20, 2.f);

                    // 8 sun rays
                    for (int r = 0; r < 8; ++r) {
                        const float angle = r * 3.14159265f * 0.25f;
                        const float cs = std::cos(angle), sn = std::sin(angle);
                        const ImVec2 inner = { lightScreen.x + cs * (kRadius + 4.f),
                                               lightScreen.y + sn * (kRadius + 4.f) };
                        const ImVec2 outer = { lightScreen.x + cs * (kRadius + 10.f),
                                               lightScreen.y + sn * (kRadius + 10.f) };
                        drawList->AddLine(inner, outer, kYellow, 2.f);
                    }

                    // Label
                    drawList->AddText({ lightScreen.x + kRadius + 4.f, lightScreen.y - 7.f },
                                      kYellow, "Light");
                }

                // Arrow from sun toward scene (showing light direction)
                if (lightOk && arrowOk) {
                    drawList->AddLine(lightScreen, arrowTip, kYellow, 2.5f);

                    // Arrowhead triangle
                    float dx = arrowTip.x - lightScreen.x;
                    float dy = arrowTip.y - lightScreen.y;
                    const float len = std::sqrt(dx*dx + dy*dy);
                    if (len > 1.f) {
                        dx /= len; dy /= len;
                        const ImVec2 left  = { arrowTip.x - dx*10.f + dy*5.f,
                                               arrowTip.y - dy*10.f - dx*5.f };
                        const ImVec2 right2 = { arrowTip.x - dx*10.f - dy*5.f,
                                                arrowTip.y - dy*10.f + dx*5.f };
                        drawList->AddTriangleFilled(arrowTip, left, right2, kYellow);
                    }
                }
            }

            bool showDirectionalLightOnSkybox = app.GetShowDirectionalLightOnSkybox();
            if (ImGui::Checkbox("Show Dir On Skybox", &showDirectionalLightOnSkybox)) {
                app.SetShowDirectionalLightOnSkybox(showDirectionalLightOnSkybox);
            }
            if (showDirectionalLightOnSkybox) {
                float directionalLightOnSkyboxAngularRadius = app.GetDirectionalLightOnSkyboxAngularRadius();
                if (ImGui::SliderAngle("Skybox Marker Size",
                                       &directionalLightOnSkyboxAngularRadius,
                                       0.1f,
                                       15.0f)) {
                    app.SetDirectionalLightOnSkyboxAngularRadius(directionalLightOnSkyboxAngularRadius);
                }
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

            // ── Point / Spot light 3D gizmos (viewport overlay) ──────────────
            ImGui::Separator();
            ImGui::Checkbox("Show Point/Spot Gizmos", &m_showLightGizmos);
            if (m_showLightGizmos) {
                const auto& gProxy = app.GetMainCameraProxy();
                const float* gvp   = gProxy.viewProjection;
                const float  gsw   = static_cast<float>(app.GetWidth());
                const float  gsh   = static_cast<float>(app.GetHeight());

                // World → screen (row-vector: clip = worldPos * VP)
                auto gw2s = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
                    const float cx = wx*gvp[0] + wy*gvp[4] + wz*gvp[8]  + gvp[12];
                    const float cy = wx*gvp[1] + wy*gvp[5] + wz*gvp[9]  + gvp[13];
                    const float cw = wx*gvp[3] + wy*gvp[7] + wz*gvp[11] + gvp[15];
                    if (std::abs(cw) < 1e-7f || cw < 0.0f) return false;
                    const float ndcX = cx / cw, ndcY = cy / cw;
                    out = { (ndcX + 1.f) * 0.5f * gsw, (1.f - ndcY) * 0.5f * gsh };
                    return ndcX >= -1.1f && ndcX <= 1.1f && ndcY >= -1.1f && ndcY <= 1.1f;
                };

                ImDrawList* gdl = ImGui::GetForegroundDrawList();

                // ── Point lights: filled circle + crosshair + label
                for (size_t pi = 0; pi < pointLights.size(); ++pi) {
                    auto* pl = pointLights[pi];
                    if (!pl) continue;
                    const auto& ppos = pl->Transform().position;
                    ImVec2 psc{};
                    if (!gw2s(ppos.x, ppos.y, ppos.z, psc)) continue;

                    const float* pc = pl->ColorData();
                    const ImU32 pcol   = IM_COL32(static_cast<int>(std::fmin(pc[0],1.f)*255.f),
                                                   static_cast<int>(std::fmin(pc[1],1.f)*255.f),
                                                   static_cast<int>(std::fmin(pc[2],1.f)*255.f), 220);
                    const ImU32 pcolBg = IM_COL32(static_cast<int>(std::fmin(pc[0],1.f)*160.f),
                                                   static_cast<int>(std::fmin(pc[1],1.f)*160.f),
                                                   static_cast<int>(std::fmin(pc[2],1.f)*160.f), 130);
                    constexpr float kPR = 10.f;
                    gdl->AddCircleFilled(psc, kPR, pcolBg, 16);
                    gdl->AddCircle(psc, kPR, pcol, 16, 2.f);
                    gdl->AddLine({psc.x - kPR*0.5f, psc.y}, {psc.x + kPR*0.5f, psc.y}, pcol, 1.5f);
                    gdl->AddLine({psc.x, psc.y - kPR*0.5f}, {psc.x, psc.y + kPR*0.5f}, pcol, 1.5f);
                    char pbuf[16];
                    std::snprintf(pbuf, sizeof(pbuf), "P%d", static_cast<int>(pi));
                    gdl->AddText({psc.x + kPR + 3.f, psc.y - 7.f}, pcol, pbuf);
                }

                // ── Spot lights: diamond + direction arrow + cone edges + label
                for (size_t si = 0; si < spotLights.size(); ++si) {
                    auto* sl = spotLights[si];
                    if (!sl) continue;
                    const auto& str = sl->Transform();
                    const auto& spos = str.position;
                    ImVec2 ssc{};
                    if (!gw2s(spos.x, spos.y, spos.z, ssc)) continue;

                    const float* sc_ = sl->ColorData();
                    const ImU32 scol   = IM_COL32(static_cast<int>(std::fmin(sc_[0],1.f)*255.f),
                                                   static_cast<int>(std::fmin(sc_[1],1.f)*255.f),
                                                   static_cast<int>(std::fmin(sc_[2],1.f)*255.f), 220);
                    const ImU32 scolBg = IM_COL32(static_cast<int>(std::fmin(sc_[0],1.f)*160.f),
                                                   static_cast<int>(std::fmin(sc_[1],1.f)*160.f),
                                                   static_cast<int>(std::fmin(sc_[2],1.f)*160.f), 130);
                    constexpr float kSR = 10.f;

                    // Diamond icon
                    const ImVec2 dtop  = {ssc.x,        ssc.y - kSR};
                    const ImVec2 drt   = {ssc.x + kSR,  ssc.y};
                    const ImVec2 dbot  = {ssc.x,        ssc.y + kSR};
                    const ImVec2 dlt   = {ssc.x - kSR,  ssc.y};
                    gdl->AddQuadFilled(dtop, drt, dbot, dlt, scolBg);
                    gdl->AddQuad(dtop, drt, dbot, dlt, scol, 2.f);

                    char sbuf[16];
                    std::snprintf(sbuf, sizeof(sbuf), "S%d", static_cast<int>(si));
                    gdl->AddText({ssc.x + kSR + 3.f, ssc.y - 7.f}, scol, sbuf);

                    // Forward direction from yaw/pitch
                    const float scy = std::cos(str.rotation.yaw), ssy = std::sin(str.rotation.yaw);
                    const float scp = std::cos(str.rotation.pitch), ssp = std::sin(str.rotation.pitch);
                    float sfx = -ssy, sfy = ssp * scy, sfz = -scp * scy;
                    const float sfl = std::sqrt(sfx*sfx + sfy*sfy + sfz*sfz);
                    if (sfl < 1e-6f) continue;
                    sfx /= sfl; sfy /= sfl; sfz /= sfl;

                    // Direction arrow
                    const float arrowDist = std::fmin(sl->Range() * 0.5f, 3.0f);
                    ImVec2 ae{};
                    if (gw2s(spos.x + sfx*arrowDist, spos.y + sfy*arrowDist, spos.z + sfz*arrowDist, ae)) {
                        gdl->AddLine(ssc, ae, scol, 2.f);
                        float adx = ae.x - ssc.x, ady = ae.y - ssc.y;
                        const float alen = std::sqrt(adx*adx + ady*ady);
                        if (alen > 1.f) {
                            adx /= alen; ady /= alen;
                            const ImVec2 ah1 = {ae.x - adx*8.f + ady*4.f, ae.y - ady*8.f - adx*4.f};
                            const ImVec2 ah2 = {ae.x - adx*8.f - ady*4.f, ae.y - ady*8.f + adx*4.f};
                            gdl->AddTriangleFilled(ae, ah1, ah2, scol);
                        }
                    }

                    // Cone edges (outer angle, 4 spokes: right/left/up/down)
                    float upX = 0.f, upY = 1.f, upZ = 0.f;
                    if (std::fabs(sfy) > 0.99f) { upX = 1.f; upY = 0.f; upZ = 0.f; }
                    float rX = sfy*upZ - sfz*upY, rY = sfz*upX - sfx*upZ, rZ = sfx*upY - sfy*upX;
                    const float rlen = std::sqrt(rX*rX + rY*rY + rZ*rZ);
                    if (rlen < 1e-6f) continue;
                    rX /= rlen; rY /= rlen; rZ /= rlen;
                    const float uX = sfy*rZ - sfz*rY, uY = sfz*rX - sfx*rZ, uZ = sfx*rY - sfy*rX;

                    const float coneLen = std::fmin(sl->Range(), 3.0f);
                    const float cosO = std::cos(sl->OuterAngle()), sinO = std::sin(sl->OuterAngle());
                    const float edgeDirs[4][3] = {
                        { sfx*cosO + rX*sinO, sfy*cosO + rY*sinO, sfz*cosO + rZ*sinO },
                        { sfx*cosO - rX*sinO, sfy*cosO - rY*sinO, sfz*cosO - rZ*sinO },
                        { sfx*cosO + uX*sinO, sfy*cosO + uY*sinO, sfz*cosO + uZ*sinO },
                        { sfx*cosO - uX*sinO, sfy*cosO - uY*sinO, sfz*cosO - uZ*sinO },
                    };
                    for (int e = 0; e < 4; ++e) {
                        ImVec2 ept{};
                        if (gw2s(spos.x + edgeDirs[e][0]*coneLen,
                                 spos.y + edgeDirs[e][1]*coneLen,
                                 spos.z + edgeDirs[e][2]*coneLen, ept)) {
                            gdl->AddLine(ssc, ept, scol, 1.5f);
                        }
                    }
                }
            }
            // ── End Point / Spot gizmos ───────────────────────────────────────

            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Rendering")) {
            int renderPathMode = app.GetRenderPathModeIndex();
            if (ImGui::Combo("Render Type", &renderPathMode, "Raster\0Hardware RT\0")) {
                app.SetRenderPathModeIndex(renderPathMode);
            }
            const bool isRasterRenderType =
                (renderPathMode == static_cast<int>(RendererEnums::RenderPathMode::Raster));
            const bool isRayTracingRenderType =
                (renderPathMode == static_cast<int>(RendererEnums::RenderPathMode::HardwareRayTracing));

            ImGui::Separator();
            if (isRasterRenderType) {
                DrawRenderPassBuilderControls(app);
                ImGui::Separator();
            }
            if (isRasterRenderType) {
                // --- Geometry pipeline comparison ---
                bool useMeshShader   = app.GetUseMeshShader();
                bool useTessellation = app.GetUseTessellation();

                ImGui::TextDisabled("Geometry Pipeline");
                if (ImGui::RadioButton("Mesh Shader (AS+MS) [default]", useMeshShader && !useTessellation)) {
                    app.SetUseMeshShader(true);
                    app.SetUseTessellation(false);
                }
                if (ImGui::RadioButton("Tessellation (VS+HS+DS+GS)", !useMeshShader && useTessellation)) {
                    app.SetUseMeshShader(false);
                    app.SetUseTessellation(true);
                }
                if (ImGui::RadioButton("Standard (VS only)", !useMeshShader && !useTessellation)) {
                    app.SetUseMeshShader(false);
                    app.SetUseTessellation(false);
                }
                ImGui::TextDisabled("LOD: <5m dense / 5-15m mid / >15m cull");

                // Debug options that are only relevant for specific pipeline modes
                if (useTessellation) {
                    bool tessWireframe = app.GetTessWireframeEnabled();
                    if (ImGui::Checkbox("Tessellation Wireframe", &tessWireframe)) {
                        app.SetTessWireframeEnabled(tessWireframe);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Render tessellated polygons as wireframe\nto visualize the geometry formed by the tessellation stage.");
                    }
                    bool tessDebug = app.GetTessDebugColorsEnabled();
                    if (ImGui::Checkbox("Tessellation Patch Colors", &tessDebug)) {
                        app.SetTessDebugColorsEnabled(tessDebug);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Flat-shade each tessellation patch with a unique\nhash-based color to visualize patch boundaries\n(similar to Meshlet Debug View).");
                    }
                }
                if (useMeshShader) {
                    bool meshletDebug = app.GetMeshletDebugViewEnabled();
                    if (ImGui::Checkbox("Meshlet Debug View", &meshletDebug)) {
                        app.SetMeshletDebugViewEnabled(meshletDebug);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Color each triangle group by its approximate meshlet index\n(SV_PrimitiveID / 16) to visualize meshlet boundaries.");
                    }
                }

                int rasterMode = app.GetRasterShaderModeIndex();
                if (ImGui::Combo("Raster Shader", &rasterMode, "Lighting (PBR)\0Opaque (Unlit)\0")) {
                    app.SetRasterShaderModeIndex(rasterMode);
                }

                int debugMode = app.GetGBufferDebugViewIndex();
                if (ImGui::Combo("GBuffer Debug", &debugMode,
                                 "Final Lit\0Albedo\0Normal\0Roughness\0Metallic\0Ambient Occlusion\0Shadow\0Emissive\0Runtime AO Raw\0Runtime AO Filtered\0Directional Light Dir\0Directional NdotL\0Reflection Radiance\0Reflection Alpha\0SWRT Reflection Hit Distance\0SWRT Reflection Composite\0")) {
                    app.SetGBufferDebugViewIndex(debugMode);
                }
                ImGui::TextDisabled("Shortcut: F2 (cycle)");

                bool softwareDirectionalShadow = app.GetRasterSoftwareRayTracedDirectionalShadowEnabled();
                if (ImGui::Checkbox("SWRT Directional Shadow", &softwareDirectionalShadow)) {
                    app.SetRasterSoftwareRayTracedDirectionalShadowEnabled(softwareDirectionalShadow);
                }
                bool softwareReflections = app.GetRasterSoftwareRayTracedReflectionEnabled();
                bool screenSpaceReflections = app.GetRasterScreenSpaceReflectionEnabled();
                int reflectionMode = softwareReflections ? 1 : (screenSpaceReflections ? 2 : 0);
                if (ImGui::Combo("Reflection Mode", &reflectionMode,
                                 "None\0Software Ray Traced\0Screen Space\0")) {
                    if (reflectionMode == 1) {
                        app.SetRasterScreenSpaceReflectionEnabled(false);
                        app.SetRasterSoftwareRayTracedReflectionEnabled(true);
                    } else if (reflectionMode == 2) {
                        app.SetRasterSoftwareRayTracedReflectionEnabled(false);
                        app.SetRasterScreenSpaceReflectionEnabled(true);
                    } else {
                        app.SetRasterSoftwareRayTracedReflectionEnabled(false);
                        app.SetRasterScreenSpaceReflectionEnabled(false);
                    }
                    softwareReflections = app.GetRasterSoftwareRayTracedReflectionEnabled();
                    screenSpaceReflections = app.GetRasterScreenSpaceReflectionEnabled();
                }
                ImGui::SetItemTooltip("Raster reflection mode. SWRT and screen-space reflections are mutually exclusive; SSR uses screen-space depth hits and IBL fallback.");
                int aoMode = app.GetAmbientOcclusionModeIndex();
                if (ImGui::Combo("AO Mode", &aoMode, "Material Only\0Runtime AO Only\0RTAO Only\0Hybrid (Material * Runtime AO)\0")) {
                    app.SetAmbientOcclusionModeIndex(aoMode);
                }
                const bool aoUsesRuntimeAo = (aoMode != 0);
                int runtimeAoMethod = app.GetRuntimeAOMethodIndex();
                const bool forceRtaoOnly = (aoMode == 2);
                if (aoUsesRuntimeAo && !forceRtaoOnly) {
                    if (ImGui::Combo("Runtime AO Method", &runtimeAoMethod, "SSAO\0RTAO\0")) {
                        app.SetRuntimeAOMethodIndex(runtimeAoMethod);
                    }
                }
                const bool aoUsesRtao = aoUsesRuntimeAo && (forceRtaoOnly || runtimeAoMethod == 1);
                const bool aoUsesSsao = aoUsesRuntimeAo && !aoUsesRtao;
                if (aoUsesRuntimeAo) {
                    float runtimeAoRadius = app.GetRuntimeAORadius();
                    if (ImGui::SliderFloat("Runtime AO Radius",
                                           &runtimeAoRadius,
                                           0.05f,
                                           3.0f)) {
                        app.SetRuntimeAORadius(runtimeAoRadius);
                    }
                    float runtimeAoBias = app.GetRuntimeAOBias();
                    if (ImGui::SliderFloat("Runtime AO Bias",
                                           &runtimeAoBias,
                                           0.001f,
                                           0.1f)) {
                        app.SetRuntimeAOBias(runtimeAoBias);
                    }
                    float runtimeAoIntensity = app.GetRuntimeAOIntensity();
                    if (ImGui::SliderFloat(aoUsesRtao ? "Runtime AO Power" : "Runtime AO Intensity",
                                           &runtimeAoIntensity,
                                           0.0f,
                                           4.0f)) {
                        app.SetRuntimeAOIntensity(runtimeAoIntensity);
                    }
                    float aoMinOcc = app.GetAoMinOcclusion();
                    if (ImGui::SliderFloat("AO Min Occlusion", &aoMinOcc, 0.0f, 1.0f,
                                           "%.2f")) {
                        app.SetAoMinOcclusion(aoMinOcc);
                    }
                    ImGui::SetItemTooltip("Minimum brightness in fully-occluded areas (0=full black, UE-style floor).");
                    float runtimeAoThickness = app.GetRuntimeAOThickness();
                    if (ImGui::SliderFloat("Runtime AO Thickness",
                                           &runtimeAoThickness,
                                           0.01f,
                                           0.75f)) {
                        app.SetRuntimeAOThickness(runtimeAoThickness);
                    }
                    if (aoUsesSsao) {
                        int ssaoQuality = app.GetRuntimeAOQualityIndex();
                        if (ImGui::Combo("SSAO Quality", &ssaoQuality, "Low\0Medium\0High\0")) {
                            app.SetRuntimeAOQualityIndex(ssaoQuality);
                        }
                    }
                    if (aoUsesRtao) {
                        const int aoSampleOptions[] = { 4, 8, 12, 16, 24, 32 };
                        int aoSampleIndex = 3;
                        int currentAoSamples = app.GetSwrtAoSampleCount();
                        for (int optionIndex = 0; optionIndex < 6; ++optionIndex) {
                            if (aoSampleOptions[optionIndex] == currentAoSamples) {
                                aoSampleIndex = optionIndex;
                                break;
                            }
                        }
                        if (ImGui::Combo("RTAO Samples",
                                         &aoSampleIndex,
                                         "4\0""8\0""12\0""16\0""24\0""32\0")) {
                            app.SetSwrtAoSampleCount(aoSampleOptions[aoSampleIndex]);
                        }
                    }
                }
                if (softwareReflections) {
                    int swrtMode = app.GetSwrtUseReSTIR() ? 1 : 0;
                    if (ImGui::Combo("SWRT Mode", &swrtMode, "Standard (NEE)\0ReSTIR DI + SVGF\0")) {
                        app.SetSwrtUseReSTIR(swrtMode == 1);
                    }
                    if (!app.GetSwrtUseReSTIR()) {
                        int samplingMode = app.GetSwrtSamplingMode();
                        if (ImGui::Combo("SWRT Sampling", &samplingMode,
                                         "IS Only\0NEE Only\0MIS (IS + NEE)\0")) {
                            app.SetSwrtSamplingMode(samplingMode);
                        }
                        const int sppValues[] = { 1, 2, 4, 8 };
                        int sppIdx = 0;
                        int curSpp = app.GetSwrtSamplesPerPixel();
                        for (int i = 0; i < 4; ++i) { if (sppValues[i] == curSpp) { sppIdx = i; break; } }
                        if (ImGui::Combo("SWRT Samples/Pixel", &sppIdx, "1 (mirror)\0""2 samples\0""4 samples\0""8 samples\0")) {
                            app.SetSwrtSamplesPerPixel(sppValues[sppIdx]);
                        }
                        const int bounceValues[] = { 1, 2, 3, 4, 8 };
                        int bounceIdx = 1;
                        int curBounces = app.GetSwrtMaxBounces();
                        for (int i = 0; i < 5; ++i) { if (bounceValues[i] == curBounces) { bounceIdx = i; break; } }
                        if (ImGui::Combo("SWRT Max Bounces", &bounceIdx, "1 bounce\0""2 bounces\0""3 bounces\0""4 bounces\0""8 bounces\0")) {
                            app.SetSwrtMaxBounces(bounceValues[bounceIdx]);
                        }
                        bool denoiserEnabled = app.GetSwrtDenoiserEnabled();
                        if (ImGui::Checkbox("SWRT Denoiser", &denoiserEnabled)) {
                            app.SetSwrtDenoiserEnabled(denoiserEnabled);
                        }
                        int atrousIterations = app.GetSwrtReflectionAtrousIterations();
                        if (ImGui::SliderInt("SWRT A-Trous Iterations", &atrousIterations, 0, 5)) {
                            app.SetSwrtReflectionAtrousIterations(atrousIterations);
                        }
                    }
                }
                if (softwareDirectionalShadow || softwareReflections || aoUsesRtao) {
                    int rayTracingPreset = app.GetRayTracingPerformancePresetIndex();
                    if (ImGui::Combo("SWRT Partial RT Preset", &rayTracingPreset, "Balanced\0Performance\0Ultra Fast\0")) {
                        app.SetRayTracingPerformancePresetIndex(rayTracingPreset);
                    }

                    const auto rayTracingStats = app.GetRayTracingStats();
                    ImGui::TextDisabled("SWRT Partial RT: %ux%u",
                                        rayTracingStats.renderWidth,
                                        rayTracingStats.renderHeight);
                    ImGui::TextDisabled("SWRT Partial RT Cost: total %.2f ms / build %.2f / trace %.2f / copy %.2f",
                                        rayTracingStats.lastFrameMs,
                                        rayTracingStats.sceneBuildMs,
                                        rayTracingStats.traceMs,
                                        rayTracingStats.copyMs);
                    ImGui::TextDisabled("SWRT Scene: %u instances / %u triangles / %u BVH nodes",
                                        rayTracingStats.instanceCount,
                                        rayTracingStats.triangleCount,
                                        rayTracingStats.bvhNodeCount);
                    ImGui::TextDisabled("SWRT Shadow: %s / cache %s / %ux%u / interval %u",
                                        rayTracingStats.shadowUpdatedThisFrame ? "updated" : "idle",
                                        rayTracingStats.shadowReusedThisFrame ? "reuse" : "fresh",
                                        rayTracingStats.shadowMapSize,
                                        rayTracingStats.shadowMapSize,
                                        rayTracingStats.shadowUpdateInterval);
                    ImGui::TextDisabled("SWRT Reflection: %s / cache %s / %ux%u / interval %u / phase %u/%u",
                                        rayTracingStats.reflectionUpdatedThisFrame ? "updated" : "idle",
                                        rayTracingStats.reflectionReusedThisFrame ? "reuse" : "fresh",
                                        rayTracingStats.reflectionWidth,
                                        rayTracingStats.reflectionHeight,
                                        rayTracingStats.reflectionUpdateInterval,
                                        rayTracingStats.reflectionPhaseCount > 0u ? (rayTracingStats.reflectionPhaseIndex + 1u) : 0u,
                                        rayTracingStats.reflectionPhaseCount);
                    ImGui::TextDisabled("SWRT Reflection Filter: roughness <= %.2f / energy >= %.2f / distance <= %.1f",
                                        rayTracingStats.reflectionMaxRoughness,
                                        rayTracingStats.reflectionMinEnergy,
                                        rayTracingStats.reflectionMaxDistance);
                }
            }

            ImGui::Separator();
            if (isRayTracingRenderType) {
                int rayTracingPreset = app.GetRayTracingPerformancePresetIndex();
                if (ImGui::Combo("RT Preset", &rayTracingPreset, "Balanced\0Performance\0Ultra Fast\0")) {
                    app.SetRayTracingPerformancePresetIndex(rayTracingPreset);
                }
                int rayTracingBounceCount = app.GetRayTracingMaxBounceCount();
                if (ImGui::SliderInt("RT Bounce Count", &rayTracingBounceCount, 1, 8)) {
                    app.SetRayTracingMaxBounceCount(rayTracingBounceCount);
                }
                ImGui::TextDisabled("1 = primary only, default = 2");
                bool dynamicResolutionEnabled = app.GetRayTracingDynamicResolutionEnabled();
                if (ImGui::Checkbox("RT Dynamic Resolution", &dynamicResolutionEnabled)) {
                    app.SetRayTracingDynamicResolutionEnabled(dynamicResolutionEnabled);
                }

                const auto rayTracingStats = app.GetRayTracingStats();
                ImGui::TextDisabled("HWRT Support: %s", app.IsHardwareRayTracingSupported() ? "Yes" : "No");
                ImGui::TextDisabled("RT Backend: %s",
                                    rayTracingStats.usingHardwarePath ? "Hardware"
                                    : "Unavailable");
                ImGui::TextDisabled("RT Scene: %u instances / %u triangles",
                                    rayTracingStats.instanceCount,
                                    rayTracingStats.triangleCount);
                ImGui::TextDisabled("RT BVH Nodes: %u", rayTracingStats.bvhNodeCount);
                ImGui::TextDisabled("RT Internal: %ux%u (%.2fx)",
                                    rayTracingStats.renderWidth,
                                    rayTracingStats.renderHeight,
                                    rayTracingStats.dynamicResolutionScale);
                ImGui::TextDisabled("RT Quality: %s",
                                    (rayTracingStats.qualityTier == 0u) ? "Full"
                                    : (rayTracingStats.qualityTier == 1u) ? "Fast"
                                    : "UltraFast");
                ImGui::TextDisabled("RT Cost: total %.2f ms / build %.2f / trace %.2f / copy %.2f",
                                    rayTracingStats.lastFrameMs,
                                    rayTracingStats.sceneBuildMs,
                                    rayTracingStats.traceMs,
                                    rayTracingStats.copyMs);
                ImGui::TextDisabled("RT Detail: primary %.2f / shadow %.2f / shade %.2f / resolve %.2f",
                                    rayTracingStats.primaryTraceMs,
                                    rayTracingStats.shadowTraceMs,
                                    rayTracingStats.shadeMs,
                                    rayTracingStats.resolveMs);
            }

            ImGui::TextDisabled("Mesh Shader Path: optional DX12 Ultimate path");
            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Common")) {
            if (app.GetDeltaTime() > 0.0f) {
                ImGui::Text("FPS: %.1f", 1.0f / app.GetDeltaTime());
            } else {
                ImGui::Text("FPS: --");
            }

            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("GI")) {
            auto& renderer = app.GetRenderer();
            auto& grid     = renderer.GetProbeGrid();

            bool giEnabled = renderer.GetGIEnabled();
            if (ImGui::Checkbox("Enable GI", &giEnabled))
                renderer.SetGIEnabled(giEnabled);

            ImGui::Separator();
            ImGui::Text("Bake Status");

            if (!grid.IsInitialized()) {
                ImGui::TextDisabled("(GI not initialized — load a scene first)");
            } else if (renderer.IsGIBaking()) {
                const float p = renderer.GetGIBakeProgress();
                ImGui::TextColored({1.f, 1.f, 0.f, 1.f}, "Baking GI...  %.0f%%", p * 100.f);
                ImGui::ProgressBar(p, {-1.f, 0.f});
                if (ImGui::Button("Cancel Bake"))
                    renderer.CancelGIBake();
            } else if (!renderer.IsGIBaked()) {
                ImGui::TextColored({1.f, 0.5f, 0.f, 1.f}, "No GI data.  Press Bake to generate.");
                if (ImGui::Button("Bake GI"))
                    renderer.RequestGIBake();
            } else {
                ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "GI data ready");
                ImGui::ProgressBar(renderer.GetGIBakeProgress(), {-1.f, 0.f});
                if (ImGui::Button("Rebuild GI"))
                    renderer.ResetAndRebakeGI();
                ImGui::SameLine();
                if (ImGui::Button("Update (continue)"))
                    renderer.RequestGIBake();
            }

            ImGui::Separator();
            ImGui::Text("Settings");
            float intensity = renderer.GetGIIntensity();
            if (ImGui::SliderFloat("GI Intensity", &intensity, 0.f, 5.f))
                renderer.SetGIIntensity(intensity);
            float ema = renderer.GetGIEmaAlpha();
            if (ImGui::SliderFloat("EMA Alpha", &ema, 0.01f, 1.f))
                renderer.SetGIEmaAlpha(ema);

            ImGui::Separator();
            ImGui::Text("Probe Grid Debug");
            bool probeDebug = app.GetDebugProbeGridEnabled();
            if (ImGui::Checkbox("Show Probe Spheres", &probeDebug))
                app.SetDebugProbeGridEnabled(probeDebug);
            if (probeDebug) {
                int probeGridPreset = m_probeGridPreset;
                if (ImGui::Combo("Grid Preset", &probeGridPreset, "Interior\0Wide\0Very Wide\0")) {
                    m_probeGridPreset = probeGridPreset;
                    ApplyProbeGridPreset(app, m_probeGridPreset);
                }
                float probeRadius = app.GetDebugProbeRadius();
                if (ImGui::SliderFloat("Probe Radius", &probeRadius, 0.05f, 2.f))
                    app.SetDebugProbeRadius(probeRadius);
            }
            ImGui::TextDisabled("Grid: %ux%ux%u  Total: %u probes",
                                grid.GetCountX(), grid.GetCountY(), grid.GetCountZ(),
                                grid.GetTotalProbeCount());
            ImGui::TextDisabled("Origin: (%.1f, %.1f, %.1f)  Spacing: %.1fm",
                                grid.GetOriginX(), grid.GetOriginY(), grid.GetOriginZ(),
                                grid.GetSpacingX());
            ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Primitives")) {
            ImGui::TextDisabled("External model assets are not required for this scene.");

            bool changed = false;
            if (m_sphereModel) {
                changed |= DrawMaterialEditor("Sphere", m_sphereMaterial);
            }
            if (m_boxModel) {
                changed |= DrawMaterialEditor("Box", m_boxMaterial);
            }
            if (m_transparentSphereModel) {
                changed |= DrawMaterialEditor("Transparent Sphere", m_transparentSphereMaterial);
            }
            if (m_transparentBoxModel) {
                changed |= DrawMaterialEditor("Transparent Box", m_transparentBoxMaterial);
            }

            if (changed) {
                if (m_sphereModel) {
                    m_sphereModel->SetMaterial(0u, m_sphereMaterial);
                }
                if (m_boxModel) {
                    m_boxModel->SetMaterial(0u, m_boxMaterial);
                }
                if (m_transparentSphereModel) {
                    m_transparentSphereModel->SetMaterial(0u, m_transparentSphereMaterial);
                }
                if (m_transparentBoxModel) {
                    m_transparentBoxModel->SetMaterial(0u, m_transparentBoxMaterial);
                }
                app.InvalidateRenderObjects();
            }
            ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        });
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiRenderer::RenderingApp, 1280, 720, L"PBR App")
