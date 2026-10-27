// ImGui panel definitions are a separate responsibility from RenderingApp's lifecycle
// (init/update/shutdown) and session-persistence logic, so RegisterUI and the helpers it
// alone uses were split out of RenderingApp.cpp into this file.

#include "RenderingApp.h"
#include "RenderingAppProbeGrid.h"
#include "ApplicationCore.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "Object/StaticModel.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UITab.h"
#include "UI/UIText.h"
#include "UI/UIButton.h"
#include "UI/UIVolume.h"
#include "UI/UIIntVolume.h"
#include "UI/UIColor.h"
#include "UI/UILayout.h"
#include "UI/UIGate.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Config/BackendFeatureSupport.h"
#include "Renderer/Runtime/Renderer.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::PBR
{
    namespace
    {
        const char* ToGIBakeStateLabel(Renderer::GIBakeState state)
        {
            switch (state) {
            case Renderer::GIBakeState::Idle: return "Idle";
            case Renderer::GIBakeState::Baking: return "Baking";
            case Renderer::GIBakeState::Completed: return "Completed";
            case Renderer::GIBakeState::Continuous: return "Continuous (DDGI)";
            case Renderer::GIBakeState::WaitingForProbeGrid: return "Waiting for probe grid";
            case Renderer::GIBakeState::WaitingForBvh: return "Waiting for software ray tracing BVH buffers";
            case Renderer::GIBakeState::Failed: return "Failed";
            default: return "Unknown";
            }
        }

        void DrawGIBakeStatus(Renderer& renderer)
        {
            const Renderer::GIBakeStatus status = renderer.GetGIBakeStatus();
            const float progress = std::clamp(status.progress, 0.0f, 1.0f);
            char progressText[64]{};
            std::snprintf(progressText, sizeof(progressText), "%.1f%%", progress * 100.0f);

            UI::Text("State: %s", ToGIBakeStateLabel(status.state));
            UI::TextDisabled("Phase: %s", status.currentPhase[0] ? status.currentPhase : "(none)");
            UI::TextDisabled("Scene: %u instances / %u triangles, versions %llu/%llu/%llu",
                             status.sceneInstances,
                             status.sceneTriangles,
                             static_cast<unsigned long long>(status.sceneGeometryVersion),
                             static_cast<unsigned long long>(status.sceneMaterialVersion),
                             static_cast<unsigned long long>(status.sceneInstanceVersion));
            ImGui::ProgressBar(progress, {-1.f, 0.f}, progressText);
            UI::TextDisabled("Probes: %u / %u", status.completedProbes, status.totalProbes);
            UI::TextDisabled("Step: %u probes/frame, remaining: %u frame(s)",
                             status.probesPerStep,
                             status.estimatedFramesRemaining);
            if (status.stalledFrames > 0u) {
                UI::TextDisabled("Stalled: %u frame(s)", status.stalledFrames);
            }

            if (status.state == Renderer::GIBakeState::WaitingForBvh) {
                ImGui::TextColored({1.f, 0.6f, 0.2f, 1.f},
                                   "Waiting: software ray tracing BVH GPU buffers are not ready.");
                if (status.bvhMissingMask != 0u) {
                    UI::TextDisabled("Missing BVH buffers:");
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_SWRT_NOT_INITIALIZED) != 0u) {
                        ImGui::BulletText("software ray tracer is not initialized");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_BVH_NODES) != 0u) {
                        ImGui::BulletText("bvhNodes");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_TRIANGLES) != 0u) {
                        ImGui::BulletText("triangles");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_MESH_INFO) != 0u) {
                        ImGui::BulletText("meshInfo");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_INSTANCES) != 0u) {
                        ImGui::BulletText("instances");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_TLAS_NODES) != 0u) {
                        ImGui::BulletText("tlasNodes");
                    }
                    if ((status.bvhMissingMask & Renderer::GI_BVH_MISSING_MATERIALS) != 0u) {
                        ImGui::BulletText("materials");
                    }
                }
            } else if (status.state == Renderer::GIBakeState::WaitingForProbeGrid) {
                ImGui::TextColored({1.f, 0.6f, 0.2f, 1.f},
                                   "Waiting: GI probe grid is not initialized.");
            } else if (status.state == Renderer::GIBakeState::Failed) {
                ImGui::TextColored({1.f, 0.3f, 0.3f, 1.f},
                                   "Failed: check the debug output for the dispatch/reallocation error.");
            }

            ImGui::SeparatorText("Bake log");
            if (status.logCount == 0u) {
                UI::TextDisabled("(no bake events)");
            } else {
                ImGui::BeginChild("GIBakeLog", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders);
                const uint32_t first = status.logCount > Renderer::GIBakeStatus::kLogCapacity
                    ? status.logCount - Renderer::GIBakeStatus::kLogCapacity
                    : 0u;
                for (uint32_t i = first; i < status.logCount; ++i) {
                    const auto& entry = status.logEntries[i];
                    UI::TextDisabled("#%u frame=%u %s", entry.sequence, entry.bakeFrame, ToGIBakeStateLabel(entry.state));
                    ImGui::SameLine();
                    UI::Text("[%s] %s", entry.phase, entry.message);
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            }
        }

        bool DrawMaterialEditor(const char* label, SurfaceMaterial& material)
        {
            bool changed = false;
            ImGui::PushID(label);
            UI::TextDisabled("%s", label);
            UI::Separator();
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
                changed |= UI::Color4("Diffuse Color", material.baseColor);
                changed |= UI::Color3("Specular Color", material.specularColor);
                float glossiness = 1.0f - material.roughness;
                if (UI::Volume("Glossiness", &glossiness, 0.0f, 1.0f)) {
                    material.roughness = 1.0f - glossiness;
                    changed = true;
                }
            } else {
                changed |= UI::Color4("Base Color", material.baseColor);
                changed |= UI::Volume("Roughness", &material.roughness, 0.0f, 1.0f);
                changed |= UI::Volume("Metallic", &material.metallic, 0.0f, 1.0f);
            }
            changed |= UI::Color3("Emissive", material.emissive);
            changed |= UI::Volume("Reflection Strength", &material.reflectionStrength, 0.0f, 1.0f);
            changed |= UI::Volume("AO Strength", &material.occlusionStrength, 0.0f, 1.0f);
            changed |= UI::Volume("Transmission", &material.transmission, 0.0f, 1.0f);
            changed |= UI::Volume("IOR", &material.ior, 1.0f, 2.5f);
            if (material.baseColor[3] < 0.999f || material.transmission > 0.0f) {
                changed |= UI::Volume("Transparent Shell", &material.transparentShellStrength, 0.0f, 2.0f);
            }
            ImGui::PopID();
            return changed;
        }

        std::optional<bool> ParseBool(const std::string& text)
        {
            if (text == "0" || text == "off" || text == "false") {
                return false;
            }
            if (text == "1" || text == "on" || text == "true") {
                return true;
            }
            return std::nullopt;
        }

        // The light gizmos used to be drawn from inside the "PBR Controls" window callback, so
        // collapsing that panel made ImGui::Begin return false and skip the callback entirely,
        // silently hiding the gizmos even though their checkboxes were still checked. Moving the
        // drawing to an overlay fixed that, but confirming the fix requires comparing screenshots
        // taken with the panel collapsed and expanded from the exact same camera pose; nudging the
        // panel by hand shifts the camera, so the toggle needs to be reachable from an automated
        // verification script instead. The flags are sample-specific state, not framework state, so
        // the commands are registered here rather than pushed into ApplicationCore.
        //
        // The handlers capture raw pointers to RenderingApp's members, which outlive the
        // registry only because RunApplication declares the application before ApplicationCore
        // (ApplicationEntryPoint.h) and so destroys the registry first. Reversing those two
        // declarations would leave these lambdas pointing at a destroyed object.
        void RegisterGizmoCommands(ApplicationCore& app, bool* showLightGizmo, bool* showLightGizmos)
        {
            Debug::DebugCommandRegistry* registry = app.GetDebugCommandRegistry();
            if (!registry) {
                return;
            }

            registry->Register("light.gizmo",
                "<0|1> - show the directional light gizmo overlay (no argument: report current)",
                [showLightGizmo](const std::vector<std::string>& args) -> std::string {
                    if (args.empty()) {
                        return std::string("OK lightGizmo=") + (*showLightGizmo ? "1" : "0");
                    }
                    const std::optional<bool> value = ParseBool(args[0]);
                    if (!value) {
                        return "ERR usage: light.gizmo <0|1>";
                    }
                    *showLightGizmo = *value;
                    return std::string("OK lightGizmo=") + (*showLightGizmo ? "1" : "0");
                });

            registry->Register("light.gizmos",
                "<0|1> - show the point/spot light gizmo overlays (no argument: report current)",
                [showLightGizmos](const std::vector<std::string>& args) -> std::string {
                    if (args.empty()) {
                        return std::string("OK lightGizmos=") + (*showLightGizmos ? "1" : "0");
                    }
                    const std::optional<bool> value = ParseBool(args[0]);
                    if (!value) {
                        return "ERR usage: light.gizmos <0|1>";
                    }
                    *showLightGizmos = *value;
                    return std::string("OK lightGizmos=") + (*showLightGizmos ? "1" : "0");
                });
        }
    }

    void RenderingApp::RegisterUI(ApplicationCore& app)
    {
        RegisterGizmoCommands(app, &m_showLightGizmo, &m_showLightGizmos);

        // The light gizmo is an overlay, not window content. It used to be drawn inside the
        // "PBR Controls" callback, which ImGuiCoordinator only invokes while ImGui::Begin
        // returns true -- so collapsing that panel silently took the gizmo with it even though
        // its own "Show Light Gizmo" checkbox was still ticked. Registered here it depends on
        // nothing but that flag.
        ImGuiCoordinator::Instance().RegisterOverlay([this, &app]() {
            if (!m_showLightGizmo) {
                return;
            }
            // Read the light here rather than capturing the window callback's local: the overlay
            // runs independently of that callback and must not depend on it having executed.
            const auto light = app.GetDirectionalLight();
            const auto& proxy = app.GetMainCameraProxy();
            const float* vp = proxy.viewProjection;
            const float sw = static_cast<float>(app.GetWidth());
            const float sh = static_cast<float>(app.GetHeight());

            // Light forward direction from yaw/pitch.
            float fDir[3] = {};
            Math::DirectionFromYawPitch(light.yaw, light.pitch, fDir);
            const float fx = fDir[0], fy = fDir[1], fz = fDir[2];

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

            // Background draw list, not the foreground one: ImGui renders the foreground
            // list after every window, so scene gizmos drawn there sit on top of the
            // control panel. The background list is drawn before windows, which keeps
            // the UI clickable and visually in front.
            ImDrawList* drawList = ImGui::GetBackgroundDrawList();
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
        });

        ImGuiCoordinator::Instance().RegisterWindow("PBR Controls", [this, &app]() {
            if (!ImGui::BeginTabBar("PBRControlTabs")) {
                return;
            }

            const auto& features = app.GetFeatureSupport();

            if (UI::Tab cameraTab{"Camera"}) {
            if (!m_camera) {
                UI::TextDisabled("Camera object is not available.");
            } else {

            float speed = m_camera->MoveSpeed();
            if (UI::Volume("Move Speed", &speed, 0.01f, 20.0f)) {
                m_camera->SetMoveSpeed(speed);
            }

            float nearClip = m_camera->NearClip();
            float farClip = m_camera->FarClip();
            bool clipChanged = false;
            clipChanged |= UI::Volume("Near Clip", &nearClip, 0.0001f, 1.0f, "%.4f", true);
            clipChanged |= UI::Volume("Far Clip", &farClip, 1.0f, 5000.0f, "%.1f", true);
            if (clipChanged) {
                m_camera->SetClipPlanes(nearClip, farClip);
            }

            const auto& transform = m_camera->Transform();
            UI::Text("Position: (%.3f, %.3f, %.3f)",
                        transform.position.x,
                        transform.position.y,
                        transform.position.z);

            const auto& proxy = app.GetMainCameraProxy();
            UI::Text("Proxy Pos: (%.3f, %.3f, %.3f)",
                        proxy.cameraPosition[0],
                        proxy.cameraPosition[1],
                        proxy.cameraPosition[2]);

            UI::Separator();
            const float yawDeg   = m_camera->Yaw()   * (180.0f / 3.14159265f);
            const float pitchDeg = m_camera->Pitch() * (180.0f / 3.14159265f);
            UI::Text("Yaw:   %.2f deg  (%.4f rad)", yawDeg,   m_camera->Yaw());
            UI::Text("Pitch: %.2f deg  (%.4f rad)", pitchDeg, m_camera->Pitch());
            UI::Text("Forward: (%.3f, %.3f, %.3f)",
                        proxy.cameraForward[0],
                        proxy.cameraForward[1],
                        proxy.cameraForward[2]);
            UI::Text("Right:   (%.3f, %.3f, %.3f)",
                        proxy.cameraRight[0],
                        proxy.cameraRight[1],
                        proxy.cameraRight[2]);
            UI::Text("Up:      (%.3f, %.3f, %.3f)",
                        proxy.cameraUp[0],
                        proxy.cameraUp[1],
                        proxy.cameraUp[2]);
            }
            }

            if (UI::Tab lightingTab{"Lighting"}) {
                DrawLightingTab(app);
            }

            if (UI::Tab renderingTab{"Rendering"}) {
                DrawRenderingTab(app);
            }

            if (UI::Tab commonTab{"Common"}) {
            if (app.GetDeltaTime() > 0.0f) {
                UI::Text("FPS: %.1f", 1.0f / app.GetDeltaTime());
            } else {
                UI::Text("FPS: --");
            }
            }

            if (UI::Tab giTab{"GI"}) {
            UI::Gate gate{features.IsSupported(RenderFeature::GlobalIllumination),
                          features.UnsupportedReason(RenderFeature::GlobalIllumination)};
            auto& renderer = app.GetRenderer();
            auto& grid     = renderer.GetProbeGrid();

            bool giEnabled = renderer.GetGIEnabled();
            if (ImGui::Checkbox("Enable GI", &giEnabled))
                renderer.SetGIEnabled(giEnabled);

            UI::Separator();
            UI::Text("Bake Status");

            if (!grid.IsInitialized()) {
                UI::TextDisabled("(GI not initialized — load a scene first)");
            } else {
                DrawGIBakeStatus(renderer);

                if (renderer.IsGIBaked()) {
                    ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "GI data ready");
                } else if (!renderer.IsGIBaking()) {
                    ImGui::TextColored({1.f, 0.5f, 0.f, 1.f}, "No GI data. Press Bake to generate.");
                }

                bool giContinuous = renderer.GetGIContinuousMode();
                if (ImGui::Checkbox("Continuous update (DDGI)", &giContinuous))
                    renderer.SetGIContinuousMode(giContinuous);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Keep re-tracing probes forever instead of stopping after one full pass, so GI tracks moving lights/geometry.");

                if (UI::Button(renderer.IsGIBaked() ? "Rebuild GI" : "Bake GI")) {
                    renderer.ResetAndRebakeGI();
                }
                ImGui::SameLine();
                if (UI::Button("Cancel Bake")) {
                    renderer.CancelGIBake();
                }
            }

            UI::Separator();
            UI::Text("Settings");
            float intensity = renderer.GetGIIntensity();
            if (UI::Volume("GI Intensity", &intensity, 0.f, 5.f))
                renderer.SetGIIntensity(intensity);
            float ema = renderer.GetGIEmaAlpha();
            if (UI::Volume("EMA Alpha", &ema, 0.01f, 1.f))
                renderer.SetGIEmaAlpha(ema);

            UI::Separator();
            UI::Text("Probe Grid Debug");
            bool probeDebug = app.GetDebugProbeGridEnabled();
            if (ImGui::Checkbox("Show Probe Spheres", &probeDebug))
                app.SetDebugProbeGridEnabled(probeDebug);
            int probeGridPreset = m_probeGridPreset;
            if (ImGui::Combo("Grid Preset", &probeGridPreset, "Interior\0Wide\0Very Wide\0Scene Auto\0")) {
                m_probeGridPreset = probeGridPreset;
                ApplyProbeGridPreset(app, m_probeGridPreset);
            }
            if (probeDebug) {
                float probeRadius = app.GetDebugProbeRadius();
                if (UI::Volume("Probe Radius", &probeRadius, 0.05f, 2.f))
                    app.SetDebugProbeRadius(probeRadius);
            }
            UI::TextDisabled("Grid: %ux%ux%u  Total: %u probes",
                             grid.GetCountX(), grid.GetCountY(), grid.GetCountZ(),
                             grid.GetTotalProbeCount());
            UI::TextDisabled("Origin: (%.1f, %.1f, %.1f)  Spacing: %.2fm",
                             grid.GetOriginX(), grid.GetOriginY(), grid.GetOriginZ(),
                             grid.GetSpacingX());
            UI::TextDisabled("Probe memory: %.1f MB", grid.GetTotalProbeCount() * 144.0f / (1024.0f * 1024.0f));

            // Probe spacing is what sets the period of the interpolation banding visible on
            // large flat surfaces, so it is the knob for that artefact. Applying it rebuilds
            // the grid and rebakes, which is why it is a button rather than a live slider.
            static float s_targetSpacing = 0.0f;
            if (s_targetSpacing <= 0.0f) {
                s_targetSpacing = grid.GetSpacingX();
            }
            UI::Volume("Target Spacing (m)", &s_targetSpacing, 0.25f, 4.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Probe count grows with 1/spacing^3 -- halving the spacing is 8x the probes\n"
                                  "and 8x the bake time. The budget below caps it.");
            }
            static int s_probeBudget = 16384;
            UI::IntVolume("Probe Budget", &s_probeBudget, 4096, 262144);
            if (UI::Button("Apply Spacing (rebake)")) {
                const float applied = renderer.SetProbeGridSpacing(s_targetSpacing, static_cast<std::uint32_t>(s_probeBudget));
                // The budget can coarsen the request; show what actually took effect.
                s_targetSpacing = applied;
            }
            }

            if (UI::Tab particleTab{"Particle"}) {
            UI::Gate gate{features.IsSupported(RenderFeature::Particles),
                          features.UnsupportedReason(RenderFeature::Particles)};
            UI::Text("Particles (V1)");
            bool particlesEnabled = app.GetParticlesEnabled();
            if (ImGui::Checkbox("Show Particles", &particlesEnabled)) app.SetParticlesEnabled(particlesEnabled);
            if (particlesEnabled) {
                float emissionRate = app.GetParticleEmissionRate();
                if (UI::Volume("Emission Rate", &emissionRate, 0.0f, 1000.0f)) app.SetParticleEmissionRate(emissionRate);
                float gravity = app.GetParticleGravity();
                if (UI::Volume("Gravity", &gravity, -20.0f, 0.0f)) app.SetParticleGravity(gravity);
                float drag = app.GetParticleDrag();
                if (UI::Volume("Drag", &drag, 0.0f, 2.0f)) app.SetParticleDrag(drag);
            }
            }

            if (UI::Tab primitivesTab{"Primitives"}) {
            UI::TextDisabled("External model assets are not required for this scene.");

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
            }

            ImGui::EndTabBar();
        });
    }
}
