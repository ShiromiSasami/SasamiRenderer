#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "Object/StaticModel.h"
#include "Object/SkinnedModel.h"
#include "Input/InputSystem.h"
#include "UI/ImGuiCoordinator.h"
#include "UI/UiTab.h"
#include "UI/UiText.h"
#include "UI/UiButton.h"
#include "UI/UiVolume.h"
#include "UI/UiLayout.h"
#include "ApplicationEntryPoint.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/Runtime/Renderer.h"
#include "ApplicationResourcePaths.h"
#include "imgui.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        constexpr const char* kSkyboxPanoramaPath = "Assets/Models/Bistro/san_giuseppe_bridge_4k.hdr";
        constexpr const char* kStanfordBunnyPath = "Models/stanford_bunny_pbr/scene.gltf";
        constexpr const char* kFoxPath = "Models/Fox/Fox.gltf";

        constexpr const char* kSponzaPath = "Models/Sponza/glTF/Sponza.gltf";
        constexpr const char* kBistroPath = "Models/Bistro/BistroExterior.fbx";

        // Session persistence file names (resolved to project root / exe dir at runtime).
        constexpr const char* kSceneStateFile    = "PBRApp.scene";
        constexpr const char* kSettingsStateFile = "PBRApp.settings.ini";
        constexpr const char* kGIProbeCacheFile  = "PBRApp.gi_probe_cache.bin";

        uint64_t Fnva1Mix(uint64_t hash, const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        uint64_t Fnva1MixString(uint64_t hash, const char* text)
        {
            return Fnva1Mix(hash, text, std::strlen(text));
        }

        uint64_t Fnva1MixFile(uint64_t hash, const std::string& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                return Fnva1MixString(hash, "<missing>");
            }

            std::array<char, 4096> buffer{};
            while (in.good()) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize readCount = in.gcount();
                if (readCount > 0) {
                    hash = Fnva1Mix(hash, buffer.data(), static_cast<size_t>(readCount));
                }
            }
            return hash;
        }

        uint64_t ComputeSessionStateHash(const std::string& scenePath,
                                         const std::string& settingsPath)
        {
            uint64_t hash = 14695981039346656037ull;
            hash = Fnva1MixString(hash, "scene:");
            hash = Fnva1MixFile(hash, scenePath);
            hash = Fnva1MixString(hash, "|settings:");
            hash = Fnva1MixFile(hash, settingsPath);
            return hash;
        }

        struct RenderPassBuilderUiEntry
        {
            const char* label = "";
            RendererEnums::RenderPassType type = RendererEnums::RenderPassType::OpaqueGBuffer;
            // Shown but not toggleable: deferred lighting reads this pass's targets, so
            // dropping it renders from targets nothing wrote. Renderer::SetRenderPassSequence
            // restores it regardless; listing it here keeps the UI honest about the graph.
            bool required = false;
        };

        constexpr std::array<RenderPassBuilderUiEntry, 15> kRenderPassBuilderUiEntries = {{
            { "Shadow", RendererEnums::RenderPassType::Shadow },
            { "Opaque GBuffer", RendererEnums::RenderPassType::OpaqueGBuffer, true },
            { "Runtime AO", RendererEnums::RenderPassType::RuntimeAO },
            { "Runtime AO Blur", RendererEnums::RenderPassType::RuntimeAOBlur },
            { "Lighting", RendererEnums::RenderPassType::Lighting },
            { "Screen Space Reflection", RendererEnums::RenderPassType::ScreenSpaceReflection },
            { "Software Reflection", RendererEnums::RenderPassType::SoftwareReflection },
            { "Software Reflection Composite", RendererEnums::RenderPassType::SoftwareReflectionComposite },
            { "Skybox", RendererEnums::RenderPassType::Skybox },
            { "Procedural Sky", RendererEnums::RenderPassType::ProceduralSky },
            { "Transparent Backface Distance", RendererEnums::RenderPassType::TransparentBackfaceDistance },
            { "Transparent Scene Color Copy", RendererEnums::RenderPassType::TransparentSceneColorCopy },
            { "Transparent Lighting", RendererEnums::RenderPassType::TransparentLighting },
            { "Transparent Composite", RendererEnums::RenderPassType::TransparentComposite },
            { "Post Process", RendererEnums::RenderPassType::PostProcess },
        }};

        bool HasRenderPass(const std::vector<ApplicationCore::RenderPassType>& sequence,
                           RendererEnums::RenderPassType type)
        {
            return std::find(sequence.begin(), sequence.end(), type) != sequence.end();
        }

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
                if (entry.required) {
                    bool alwaysOn = true;
                    ImGui::BeginDisabled();
                    ImGui::Checkbox(entry.label, &alwaysOn);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    UI::TextDisabled("(required by deferred lighting)");
                    nextSequence.push_back(entry.type);
                    continue;
                }
                if (ImGui::Checkbox(entry.label, &enabled)) {
                    changed = true;
                }
                if (enabled) {
                    nextSequence.push_back(entry.type);
                }
            }

            if (UI::Button("Reset Passes")) {
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
            case 3:
                // Scene auto-fit: covers every loaded model (Sponza and the Bistro block at
                // x = 60 alike) instead of a hardcoded Sponza-sized box, coarsening probe
                // spacing as needed to stay inside the budget. Falls back to the default
                // interior box while the scene has no geometry yet.
                if (!app.FitProbeGridToSceneAuto(2.0f, 16384u)) {
                    app.FitProbeGridToScene(-12.0f, -1.0f, -18.0f, 12.0f, 12.0f, 20.0f, 2.0f);
                }
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
            material.reflectionStrength = Math::DefaultReflectionStrength(roughness, metallic);
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
                changed |= ImGui::ColorEdit4("Diffuse Color", material.baseColor);
                changed |= ImGui::ColorEdit3("Specular Color", material.specularColor);
                float glossiness = 1.0f - material.roughness;
                if (UI::Volume("Glossiness", &glossiness, 0.0f, 1.0f)) {
                    material.roughness = 1.0f - glossiness;
                    changed = true;
                }
            } else {
                changed |= ImGui::ColorEdit4("Base Color", material.baseColor);
                changed |= UI::Volume("Roughness", &material.roughness, 0.0f, 1.0f);
                changed |= UI::Volume("Metallic", &material.metallic, 0.0f, 1.0f);
            }
            changed |= ImGui::ColorEdit3("Emissive", material.emissive);
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
    }

    void RenderingApp::SaveSessionState(ApplicationCore& app) const
    {
        // Camera + lights via the framework scene writer.
        const std::string scenePath =
            ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
        app.SaveScene(scenePath);

        // Render settings + GI toggles + app UI settings.
        const std::string path = ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
        std::ofstream out(path);
        if (!out.is_open()) {
            DebugLog("PBRApp: failed to open settings state file for writing.\n");
            return;
        }

        Renderer& renderer = app.GetRenderer();
        const RenderSettings& s = renderer.GetRenderSettings();

        out << "# SasamiRenderer PBRApp settings v1\n";
        out << "[render_settings]\n";
#define WRITE_F(field) out << #field " = " << s.field << '\n'
#define WRITE_U(field) out << #field " = " << s.field << '\n'
#define WRITE_B(field) out << #field " = " << (s.field ? 1 : 0) << '\n'
#define WRITE_E(field) out << #field " = " << static_cast<int>(s.field) << '\n'
        WRITE_F(iblIntensity);
        WRITE_B(useTessellation);
        WRITE_B(tessWireframeEnabled);
        WRITE_B(tessDebugColorsEnabled);
        WRITE_B(meshletDebugViewEnabled);
        WRITE_B(useMeshShader);
        WRITE_E(renderPathMode);
        WRITE_E(rayTracingPerformancePreset);
        WRITE_B(rayTracingDynamicResolutionEnabled);
        WRITE_U(rayTracingMaxBounceCount);
        WRITE_B(rasterSoftwareRayTracedDirectionalShadowEnabled);
        WRITE_B(rasterSoftwareRayTracedReflectionEnabled);
        WRITE_B(rasterScreenSpaceReflectionEnabled);
        WRITE_F(ssrMaxDistance);
        WRITE_F(ssrThickness);
        WRITE_F(ssrStepCount);
        WRITE_F(ssrRoughnessCutoff);
        WRITE_F(ssrRefineSteps);
        WRITE_F(ssrEdgeFade);
        WRITE_F(ssrNormalOffset);
        WRITE_F(ssrIntensity);
        WRITE_B(rasterSoftwareRayTracedAmbientOcclusionEnabled);
        WRITE_E(ambientOcclusionMode);
        WRITE_E(runtimeAoMethod);
        WRITE_B(swrtUseReSTIR);
        WRITE_U(swrtSamplingMode);
        WRITE_U(swrtSamplesPerPixel);
        WRITE_U(swrtMaxBounces);
        WRITE_B(swrtDenoiserEnabled);
        WRITE_F(swrtReflectionTemporalAlpha);
        WRITE_U(swrtReflectionAtrousIterations);
        WRITE_F(swrtReflectionAtrousPhiDepth);
        WRITE_B(volumetricCloudEnabled);
        WRITE_F(cloudCover);
        WRITE_F(cloudDensity);
        WRITE_F(cloudWindSpeed);
        WRITE_F(cloudBaseAlt);
        WRITE_F(cloudTopAlt);
        WRITE_B(runtimeAoEnabled);
        WRITE_F(runtimeAoRadius);
        WRITE_F(runtimeAoBias);
        WRITE_F(runtimeAoIntensity);
        WRITE_F(aoMinOcclusion);
        WRITE_F(aoDirectLightingStrength);
        WRITE_F(runtimeAoThickness);
        WRITE_U(runtimeAoQuality);
        WRITE_U(swrtAoSampleCount);
        WRITE_E(gBufferDebugView);
        WRITE_F(hardwareRayTracingResolutionScale);
        WRITE_B(vsmBlurEnabled);
        WRITE_F(exposure);
#undef WRITE_F
#undef WRITE_U
#undef WRITE_B
#undef WRITE_E

        out << "[gi]\n";
        out << "enabled = "   << (renderer.GetGIEnabled() ? 1 : 0) << '\n';
        out << "intensity = " << renderer.GetGIIntensity()         << '\n';
        out << "ema_alpha = " << renderer.GetGIEmaAlpha()          << '\n';
        out << "baked = "     << (renderer.HasGIProbeData() ? 1 : 0) << '\n';
        out << "continuous_mode = " << (renderer.GetGIContinuousMode() ? 1 : 0) << '\n';

        out << "[app]\n";
        out << "probe_preset = "      << m_probeGridPreset            << '\n';
        out << "show_probe_spheres = " << (app.GetDebugProbeGridEnabled() ? 1 : 0) << '\n';
        out << "probe_radius = "      << app.GetDebugProbeRadius()     << '\n';
        out << "probe_cache = "       << kGIProbeCacheFile             << '\n';
        out << "show_light_gizmo = "  << (m_showLightGizmo ? 1 : 0)   << '\n';
        out << "show_light_gizmos = " << (m_showLightGizmos ? 1 : 0)  << '\n';
        out << "show_particles = " << (app.GetParticlesEnabled() ? 1 : 0) << '\n';
        out.close();

        const uint64_t stateHash = ComputeSessionStateHash(scenePath, path);
        const std::string cachePath =
            ApplicationResourcePaths::ResolveConfigPathString(kGIProbeCacheFile);
        if (renderer.HasGIProbeData() && !renderer.SaveGIProbeCache(cachePath, stateHash)) {
            DebugLog("PBRApp: failed to save GI probe cache.\n");
        }
    }

    void RenderingApp::LoadSessionState(ApplicationCore& app)
    {
        const std::string settingsPath =
            ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
        const std::string scenePath =
            ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
        m_sessionGiBaked = false;

        std::ifstream in(settingsPath);
        if (in.is_open()) {
            std::map<std::string, std::map<std::string, std::string>> sections;
            std::string section;
            std::string line;
            while (std::getline(in, line)) {
                const auto trim = [](std::string& v) {
                    const auto b = v.find_first_not_of(" \t\r\n");
                    const auto e = v.find_last_not_of(" \t\r\n");
                    v = (b == std::string::npos) ? std::string{} : v.substr(b, e - b + 1);
                };
                trim(line);
                if (line.empty() || line[0] == '#') continue;
                if (line[0] == '[') {
                    const auto close = line.find(']');
                    section = (close != std::string::npos) ? line.substr(1, close - 1) : "";
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key);
                trim(val);
                if (!key.empty()) sections[section][key] = val;
            }

            const auto find = [&](const std::string& sec, const char* key) -> const std::string* {
                auto si = sections.find(sec);
                if (si == sections.end()) return nullptr;
                auto ki = si->second.find(key);
                return (ki == si->second.end()) ? nullptr : &ki->second;
            };
            const auto getF = [&](const std::string& sec, const char* key, float def) {
                const std::string* v = find(sec, key);
                if (!v) return def;
                try { return std::stof(*v); } catch (...) { return def; }
            };
            const auto getI = [&](const std::string& sec, const char* key, long def) {
                const std::string* v = find(sec, key);
                if (!v) return def;
                try { return std::stol(*v); } catch (...) { return def; }
            };
            const auto getB = [&](const std::string& sec, const char* key, bool def) {
                return getI(sec, key, def ? 1 : 0) != 0;
            };

            Renderer& renderer = app.GetRenderer();
            RenderSettings s = renderer.GetRenderSettings(); // start from current defaults
#define READ_F(field) s.field = getF("render_settings", #field, s.field)
#define READ_U(field) s.field = static_cast<uint32_t>(getI("render_settings", #field, static_cast<long>(s.field)))
#define READ_B(field) s.field = getB("render_settings", #field, s.field)
#define READ_E(field) s.field = static_cast<decltype(s.field)>(getI("render_settings", #field, static_cast<long>(s.field)))
            READ_F(iblIntensity);
            READ_B(useTessellation);
            READ_B(tessWireframeEnabled);
            READ_B(tessDebugColorsEnabled);
            READ_B(meshletDebugViewEnabled);
            READ_B(useMeshShader);
            READ_E(renderPathMode);
            READ_E(rayTracingPerformancePreset);
            READ_B(rayTracingDynamicResolutionEnabled);
            READ_U(rayTracingMaxBounceCount);
            READ_B(rasterSoftwareRayTracedDirectionalShadowEnabled);
            READ_B(rasterSoftwareRayTracedReflectionEnabled);
            READ_B(rasterScreenSpaceReflectionEnabled);
            READ_F(ssrMaxDistance);
            READ_F(ssrThickness);
            READ_F(ssrStepCount);
            READ_F(ssrRoughnessCutoff);
            READ_F(ssrRefineSteps);
            READ_F(ssrEdgeFade);
            READ_F(ssrNormalOffset);
            READ_F(ssrIntensity);
            READ_B(rasterSoftwareRayTracedAmbientOcclusionEnabled);
            READ_E(ambientOcclusionMode);
            READ_E(runtimeAoMethod);
            READ_B(swrtUseReSTIR);
            READ_U(swrtSamplingMode);
            READ_U(swrtSamplesPerPixel);
            READ_U(swrtMaxBounces);
            READ_B(swrtDenoiserEnabled);
            READ_F(swrtReflectionTemporalAlpha);
            READ_U(swrtReflectionAtrousIterations);
            READ_F(swrtReflectionAtrousPhiDepth);
            READ_B(volumetricCloudEnabled);
            READ_F(cloudCover);
            READ_F(cloudDensity);
            READ_F(cloudWindSpeed);
            READ_F(cloudBaseAlt);
            READ_F(cloudTopAlt);
            READ_B(runtimeAoEnabled);
            READ_F(runtimeAoRadius);
            READ_F(runtimeAoBias);
            READ_F(runtimeAoIntensity);
            READ_F(aoMinOcclusion);
            READ_F(aoDirectLightingStrength);
            READ_F(runtimeAoThickness);
            READ_U(runtimeAoQuality);
            READ_U(swrtAoSampleCount);
            READ_E(gBufferDebugView);
            READ_F(hardwareRayTracingResolutionScale);
            READ_B(vsmBlurEnabled);
            READ_F(exposure);
#undef READ_F
#undef READ_U
#undef READ_B
#undef READ_E
            renderer.SetRenderSettings(s);

            renderer.SetGIEnabled(getB("gi", "enabled", renderer.GetGIEnabled()));
            renderer.SetGIIntensity(getF("gi", "intensity", renderer.GetGIIntensity()));
            renderer.SetGIEmaAlpha(getF("gi", "ema_alpha", renderer.GetGIEmaAlpha()));
            renderer.SetGIContinuousMode(getB("gi", "continuous_mode", renderer.GetGIContinuousMode()));
            m_sessionGiBaked = getB("gi", "baked", false);

            m_probeGridPreset = std::clamp(static_cast<int>(getI("app", "probe_preset", m_probeGridPreset)), 0, 3);
            app.SetDebugProbeGridEnabled(getB("app", "show_probe_spheres", app.GetDebugProbeGridEnabled()));
            app.SetDebugProbeRadius(getF("app", "probe_radius", app.GetDebugProbeRadius()));
            m_showLightGizmo  = getB("app", "show_light_gizmo", m_showLightGizmo);
            m_showLightGizmos = getB("app", "show_light_gizmos", m_showLightGizmos);
            app.SetParticlesEnabled(getB("app", "show_particles", app.GetParticlesEnabled()));
        }

        // Camera + lights (non-destructive: applied onto existing objects; overrides
        // the default view/lights when a saved scene file is present).
        app.ApplyCameraAndLights(scenePath);

        // GI probe grid fit + probe cache load / bake request need final scene bounds
        // (Sponza/bunny async loads settled) and renderer GI/SWRT readiness. Apply now
        // if those conditions already hold (e.g. a manual/UI-triggered reload after the
        // app is fully up), otherwise defer to OnUpdate via m_pendingGiSessionRestore.
        Renderer& renderer = app.GetRenderer();
        const RendererReadyState& readyState = renderer.GetReadyState();
        const bool sponzaSettled = !m_sponzaModel ||
            m_sponzaModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        const bool bunnySettled = !m_bunnyModel ||
            m_bunnyModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        const bool bistroSettled = !m_bistroModel ||
            m_bistroModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        if (readyState.IsFeatureReady(readyState.giReady) &&
            readyState.IsFeatureReady(readyState.swrtReady) &&
            sponzaSettled && bunnySettled && bistroSettled) {
            ApplyDeferredGiSessionState(app);
            m_pendingGiSessionRestore = false;
        } else {
            m_pendingGiSessionRestore = true;
        }
    }

    void RenderingApp::ApplyDeferredGiSessionState(ApplicationCore& app)
    {
        ApplyProbeGridPreset(app, m_probeGridPreset);

        if (m_sessionGiBaked) {
            Renderer& renderer = app.GetRenderer();
            const std::string scenePath =
                ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
            const std::string settingsPath =
                ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
            const std::string cachePath =
                ApplicationResourcePaths::ResolveConfigPathString(kGIProbeCacheFile);
            const uint64_t stateHash = ComputeSessionStateHash(scenePath, settingsPath);
            if (!renderer.LoadGIProbeCache(cachePath, stateHash) && renderer.GetGIEnabled()) {
                renderer.RequestGIBake();
            }
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

            if (!app.LoadSkyboxAsync(kSkyboxPanoramaPath, ApplicationCore::SkyboxLoadFormat::HdrEquirect)) {
                DebugLog("Skybox load failed: invalid path for selected format.\n");
            }
        } else {
            DebugLog("PBRApp: non-DX12 backend uses native mesh frame path; DX12 render graph skybox/shadow/post passes are skipped.\n");
        }

        m_sphereMaterial = MakeMaterial(0.98f, 0.84f, 0.32f, 0.05f, 1.0f);
        m_boxMaterial = MakeMaterial(0.72f, 0.80f, 0.90f, 0.14f, 1.0f);
        m_transparentSphereMaterial = MakeTransparentMaterial(0.35f, 0.78f, 0.95f, 0.35f, 0.04f, 0.0f, 0.92f, 1.33f);
        m_transparentBoxMaterial = MakeTransparentMaterial(0.92f, 0.58f, 0.26f, 0.42f, 0.08f, 0.0f, 0.78f, 1.50f);

        m_bunnyModel = app.CreateStaticModel();
        m_sponzaModel = app.CreateStaticModel();
        m_bistroModel = app.CreateStaticModel();
        m_sphereModel = app.CreateStaticModel();
        m_boxModel = app.CreateStaticModel();
        m_transparentSphereModel = app.CreateStaticModel();
        m_transparentBoxModel = app.CreateStaticModel();
        StaticModel* floorModel = app.CreateStaticModel();

        if (!m_bunnyModel || !m_sponzaModel || !m_bistroModel || !m_sphereModel || !m_boxModel ||
            !m_transparentSphereModel || !m_transparentBoxModel || !floorModel) {
            DebugLog("Sample scene object creation failed.\n");
            app.RequestQuit();
            return;
        }

        if (!m_bunnyModel->LoadModelAsync(app, kStanfordBunnyPath, StaticModel::ModelFormat::Gltf, 0.01f)) {
            app.DeleteObject(m_bunnyModel);
            m_bunnyModel = nullptr;
            DebugLog("Bunny model load failed: Assets/Models/stanford_bunny_pbr/scene.gltf\n");
        } else {
            m_bunnyModel->SetTranslation(0.0f, 0.0f, 0.8f);
        }

        if (!m_sponzaModel->LoadModelAsync(app, kSponzaPath, StaticModel::ModelFormat::Gltf, 1.f)) {
            app.DeleteObject(m_sponzaModel);
            m_sponzaModel = nullptr;
            DebugLog("Sponza model load failed: Assets/Models/Sponza/glTF/Sponza.gltf\n");
        } else {
            m_sponzaModel->SetTranslation(0.f, 0.f, 1.f);
        }

        // Loaded synchronously, unlike the glTF models: parsing this FBX on a worker
        // thread reliably kills the process a few seconds later (no exception reaches any
        // handler), while the identical parse on the main thread is stable. Tracked as a
        // follow-up in TODO.md; until it is understood, correctness beats overlap here.
        if (!m_bistroModel->LoadModel(kBistroPath, StaticModel::ModelFormat::Fbx, 1.0f)) {
            app.DeleteObject(m_bistroModel);
            m_bistroModel = nullptr;
            DebugLog("Bistro model load failed: Assets/Models/Bistro/BistroExterior.fbx\n");
        } else {
            // +X offset keeps the Bistro exterior from overlapping Sponza, which stays at the origin.
            m_bistroModel->SetTranslation(60.0f, 0.0f, 0.0f);
        }

        // Skinned animated model (glTF sample "Fox", authored in centimeters).
        SkinnedModel* foxModel = app.CreateSkinnedModel();
        if (foxModel) {
            if (!foxModel->LoadModel(kFoxPath)) {
                app.DeleteObject(foxModel);
                foxModel = nullptr;
                DebugLog("Fox model load failed: Assets/Models/Fox/Fox.gltf\n");
            } else {
                foxModel->SetUniformScale(0.02f);
                foxModel->SetTranslation(1.2f, 0.0f, 0.3f);
                foxModel->PlayAnimation(2); // Run — clearly visible loop
            }
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

        if (!m_sponzaModel) {
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

        // Probe grid fit is deferred (see ApplyDeferredGiSessionState): scene bounds are
        // not final until the async Sponza/bunny loads settle.

        app.SetParticleEmitOrigin(0.0f, 1.5f, 0.0f);

        BindInputEvents(app);
        RegisterUi(app);

        // Restore camera, lights and settings saved from a previous run (if any).
        // Applied last so it overrides the default view/lights/settings above.
        LoadSessionState(app);
    }

    void RenderingApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        if (m_pendingGiSessionRestore && app.IsRendererReady()) {
            const RendererReadyState& readyState = app.GetRenderer().GetReadyState();
            const bool sponzaSettled = !m_sponzaModel ||
                m_sponzaModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
            const bool bunnySettled = !m_bunnyModel ||
                m_bunnyModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
            const bool bistroSettled = !m_bistroModel ||
                m_bistroModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
            if (readyState.IsFeatureReady(readyState.giReady) &&
                readyState.IsFeatureReady(readyState.swrtReady) &&
                sponzaSettled && bunnySettled && bistroSettled) {
                ApplyDeferredGiSessionState(app);
                m_pendingGiSessionRestore = false;
                DebugLog("[PBRApp] deferred GI session state applied\n");
            }
        }

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
        // Persist camera, lights and settings before the scene objects are destroyed.
        SaveSessionState(app);

        app.ClearObjects();
        m_camera = nullptr;
        m_sphereModel = nullptr;
        m_boxModel = nullptr;
        m_transparentSphereModel = nullptr;
        m_transparentBoxModel = nullptr;
        m_bunnyModel = nullptr;
        m_sponzaModel = nullptr;
        m_bistroModel = nullptr;
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
            auto light = app.GetDirectionalLight();
            bool changed = false;
            changed |= ImGui::SliderAngle("Yaw", &light.yaw, -180.0f, 180.0f);
            changed |= ImGui::SliderAngle("Pitch", &light.pitch, -180.0f, 180.0f);
            changed |= UI::Volume("Distance", &light.distance, 0.1f, 50.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Shadow camera distance from the scene center.\nThis is not physical light falloff distance.");
            }
            changed |= UI::Volume("Ortho Half", &light.orthoHalf, 0.1f, 50.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Half-size of the directional shadow orthographic projection.\nLarger values cover more area but reduce shadow texel density.");
            }
            changed |= UI::Volume("Near", &light.nearZ, 0.01f, 100.0f);
            changed |= UI::Volume("Far", &light.farZ, 0.02f, 300.0f);
            changed |= ImGui::ColorEdit4("Dir Color", light.color.Data());
            changed |= UI::Volume("Dir Intensity", &light.intensity, 0.0f, 10.0f);
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
            changed |= UI::Volume("Shadow Distance", &light.shadowDistance, 5.0f, 250.0f);
            changed |= UI::Volume("Cascade Exponent", &light.cascadeDistributionExponent, 1.0f, 4.0f);
            changed |= UI::Volume("Cascade Blend", &light.cascadeBlendFraction, 0.0f, 0.3f);
            changed |= UI::Volume("Depth Bias", &light.depthBias, 0.0f, 4000.0f);
            changed |= UI::Volume("Slope Bias", &light.slopeScaleBias, 0.0f, 8.0f);
            changed |= UI::Volume("Normal Bias", &light.normalBias, 0.0f, 0.1f);
            changed |= UI::Volume("Far Bias Scale", &light.farBiasScale, 1.0f, 4.0f);

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

            UI::Separator();
            ImGui::Checkbox("Show Light Gizmo", &m_showLightGizmo);

            // 3D light position/direction gizmo drawn on the viewport
            if (m_showLightGizmo) {
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

            UI::Separator();
            auto pointLights = app.GetPointLightObjects();
            UI::Text("Point Lights: %d", static_cast<int>(pointLights.size()));
            if (UI::Button("Add Point")) {
                app.CreatePointLightObject();
                pointLights = app.GetPointLightObjects();
            }
            ImGui::SameLine();
            if (UI::Button("Remove Point") && !pointLights.empty()) {
                app.DeleteObject(pointLights.back());
                pointLights = app.GetPointLightObjects();
            }
            for (size_t i = 0; i < pointLights.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                UI::Text("Point %d", static_cast<int>(i));
                auto* pl = pointLights[i];
                if (!pl) {
                    ImGui::PopID();
                    continue;
                }
                auto& tr = pl->Transform();
                ImGui::DragFloat3("Pos", &tr.position.x, 0.05f);
                ImGui::ColorEdit4("Color", pl->ColorData());
                float intensity = pl->Intensity();
                if (UI::Volume("Intensity", &intensity, 0.0f, 10.0f)) {
                    pl->SetIntensity(intensity);
                }
                float range = pl->Range();
                if (UI::Volume("Range", &range, 0.1f, 50.0f)) {
                    if (range < 0.01f) {
                        range = 0.01f;
                    }
                    pl->SetRange(range);
                }
                ImGui::PopID();
            }

            UI::Separator();
            auto spotLights = app.GetSpotLightObjects();
            UI::Text("Spot Lights: %d", static_cast<int>(spotLights.size()));
            if (UI::Button("Add Spot")) {
                app.CreateSpotLightObject();
                spotLights = app.GetSpotLightObjects();
            }
            ImGui::SameLine();
            if (UI::Button("Remove Spot") && !spotLights.empty()) {
                app.DeleteObject(spotLights.back());
                spotLights = app.GetSpotLightObjects();
            }
            for (size_t i = 0; i < spotLights.size(); ++i) {
                ImGui::PushID(100 + static_cast<int>(i));
                UI::Text("Spot %d", static_cast<int>(i));
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
                if (UI::Volume("Intensity", &intensity, 0.0f, 10.0f)) {
                    sl->SetIntensity(intensity);
                }
                float range = sl->Range();
                if (UI::Volume("Range", &range, 0.1f, 50.0f)) {
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
            UI::Separator();
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

                // Background draw list, not the foreground one: ImGui renders the foreground
                // list after every window, so scene gizmos drawn there sit on top of the
                // control panel. The background list is drawn before windows, which keeps
                // the UI clickable and visually in front.
                ImDrawList* gdl = ImGui::GetBackgroundDrawList();

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
                    float sDir[3] = {};
                    Math::DirectionFromYawPitch(str.rotation.yaw, str.rotation.pitch, sDir);
                    const float sfx = sDir[0], sfy = sDir[1], sfz = sDir[2];

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
            }

            if (UI::Tab renderingTab{"Rendering"}) {
            int renderPathMode = app.GetRenderPathModeIndex();
            if (ImGui::Combo("Render Type", &renderPathMode, "Raster\0Hardware RT\0")) {
                app.SetRenderPathModeIndex(renderPathMode);
            }
            const bool isRasterRenderType =
                (renderPathMode == static_cast<int>(RendererEnums::RenderPathMode::Raster));
            const bool isRayTracingRenderType =
                (renderPathMode == static_cast<int>(RendererEnums::RenderPathMode::HardwareRayTracing));

            float exposure = app.GetExposure();
            if (UI::Volume("Exposure", &exposure, 0.1f, 8.0f, "%.2f", true)) {
                app.SetExposure(exposure);
            }

            UI::Separator();
            if (isRasterRenderType) {
                DrawRenderPassBuilderControls(app);
                UI::Separator();
            }
            if (isRasterRenderType) {
                // --- Geometry pipeline comparison ---
                bool useMeshShader   = app.GetUseMeshShader();
                bool useTessellation = app.GetUseTessellation();

                UI::TextDisabled("Geometry Pipeline");
                // useMeshShader is currently inert: no render-time code reads it, the
                // MeshShaderRenderPass is never constructed, and meshlet building is not
                // implemented in any loader, so this option selects the standard VS path.
                // Labelled honestly rather than as "[default]" until the path is built.
                // See TODO.md for the wire-up-vs-delete decision.

                if (ImGui::RadioButton("Standard VS (mesh shader path not implemented)", useMeshShader && !useTessellation)) {
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
                UI::TextDisabled("LOD: <5m dense / 5-15m mid / >15m cull");

                // Debug options that are only relevant for specific pipeline modes
                if (useTessellation) {
                    bool tessWireframe = app.GetTessWireframeEnabled();
                    if (ImGui::Checkbox("Tessellation Wireframe", &tessWireframe)) {
                        app.SetTessWireframeEnabled(tessWireframe);
                    }
                    ImGui::SameLine();
                    UI::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Render tessellated polygons as wireframe\nto visualize the geometry formed by the tessellation stage.");
                    }
                    bool tessDebug = app.GetTessDebugColorsEnabled();
                    if (ImGui::Checkbox("Tessellation Patch Colors", &tessDebug)) {
                        app.SetTessDebugColorsEnabled(tessDebug);
                    }
                    ImGui::SameLine();
                    UI::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Flat-shade each tessellation patch with a unique\nhash-based color to visualize patch boundaries\n(similar to Meshlet Debug View).");
                    }
                }
                if (useMeshShader) {
                    bool meshletDebug = app.GetMeshletDebugViewEnabled();
                    if (ImGui::Checkbox("Meshlet Debug View (no effect)", &meshletDebug)) {
                        app.SetMeshletDebugViewEnabled(meshletDebug);
                    }
                    ImGui::SameLine();
                    UI::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Color each triangle group by its approximate meshlet index\n(SV_PrimitiveID / 16) to visualize meshlet boundaries.");
                    }
                }

                int debugMode = app.GetGBufferDebugViewIndex();
                if (ImGui::Combo("GBuffer Debug", &debugMode,
                                 "Final Lit\0Albedo\0Normal\0Roughness\0Metallic\0Ambient Occlusion\0Shadow\0Emissive\0Runtime AO Raw\0Runtime AO Filtered\0Directional Light Dir\0Directional NdotL\0Reflection Radiance\0Reflection Alpha\0SWRT Reflection Hit Distance\0SWRT Reflection Composite\0")) {
                    app.SetGBufferDebugViewIndex(debugMode);
                }
                UI::TextDisabled("Shortcut: F2 (cycle)");

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
                ImGui::SetItemTooltip("Raster reflection mode. SWRT and screen-space reflections are mutually exclusive; SSR ray-marches the screen-space depth buffer only (misses are dropped, no IBL fallback).");
                if (screenSpaceReflections) {
                    float ssrMaxDistance = app.GetSSRMaxDistance();
                    if (UI::Volume("SSR Max Distance", &ssrMaxDistance, 1.0f, 100.0f))
                        app.SetSSRMaxDistance(ssrMaxDistance);
                    float ssrThickness = app.GetSSRThickness();
                    if (UI::Volume("SSR Thickness", &ssrThickness, 0.01f, 2.0f))
                        app.SetSSRThickness(ssrThickness);
                    float ssrStepCount = app.GetSSRStepCount();
                    if (UI::Volume("SSR Step Count", &ssrStepCount, 8.0f, 128.0f))
                        app.SetSSRStepCount(ssrStepCount);
                    float ssrRoughnessCutoff = app.GetSSRRoughnessCutoff();
                    if (UI::Volume("SSR Roughness Cutoff", &ssrRoughnessCutoff, 0.0f, 1.0f))
                        app.SetSSRRoughnessCutoff(ssrRoughnessCutoff);
                    float ssrRefineSteps = app.GetSSRRefineSteps();
                    if (UI::Volume("SSR Refine Steps", &ssrRefineSteps, 0.0f, 8.0f))
                        app.SetSSRRefineSteps(ssrRefineSteps);
                    float ssrEdgeFade = app.GetSSREdgeFade();
                    if (UI::Volume("SSR Edge Fade", &ssrEdgeFade, 0.0f, 0.5f))
                        app.SetSSREdgeFade(ssrEdgeFade);
                    float ssrNormalOffset = app.GetSSRNormalOffset();
                    if (UI::Volume("SSR Normal Offset", &ssrNormalOffset, 0.0f, 0.5f))
                        app.SetSSRNormalOffset(ssrNormalOffset);
                    float ssrIntensity = app.GetSSRIntensity();
                    if (UI::Volume("SSR Intensity", &ssrIntensity, 0.0f, 2.0f))
                        app.SetSSRIntensity(ssrIntensity);
                }
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
                    if (UI::Volume("Runtime AO Radius",
                                   &runtimeAoRadius,
                                   0.05f,
                                   3.0f)) {
                        app.SetRuntimeAORadius(runtimeAoRadius);
                    }
                    float runtimeAoBias = app.GetRuntimeAOBias();
                    if (UI::Volume("Runtime AO Bias",
                                   &runtimeAoBias,
                                   0.001f,
                                   0.1f)) {
                        app.SetRuntimeAOBias(runtimeAoBias);
                    }
                    float runtimeAoIntensity = app.GetRuntimeAOIntensity();
                    if (UI::Volume(aoUsesRtao ? "Runtime AO Power" : "Runtime AO Intensity",
                                   &runtimeAoIntensity,
                                   0.0f,
                                   4.0f)) {
                        app.SetRuntimeAOIntensity(runtimeAoIntensity);
                    }
                    float aoMinOcc = app.GetAoMinOcclusion();
                    if (UI::Volume("AO Min Occlusion", &aoMinOcc, 0.0f, 1.0f,
                                   "%.2f")) {
                        app.SetAoMinOcclusion(aoMinOcc);
                    }
                    ImGui::SetItemTooltip("Minimum brightness in fully-occluded areas (0=full black, UE-style floor).");
                    float aoDirectStrength = app.GetAoDirectLightingStrength();
                    if (UI::Volume("AO Direct Lighting", &aoDirectStrength, 0.0f, 1.0f,
                                   "%.2f")) {
                        app.SetAoDirectLightingStrength(aoDirectStrength);
                    }
                    ImGui::SetItemTooltip("How much AO darkens direct lighting (non-physical, fakes occlusion for unshadowed point/spot lights).");
                    float runtimeAoThickness = app.GetRuntimeAOThickness();
                    if (UI::Volume("Runtime AO Thickness",
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
                    UI::TextDisabled("SWRT Partial RT: %ux%u",
                                     rayTracingStats.renderWidth,
                                     rayTracingStats.renderHeight);
                    UI::TextDisabled("SWRT Partial RT Cost: total %.2f ms / build %.2f / trace %.2f / copy %.2f",
                                     rayTracingStats.lastFrameMs,
                                     rayTracingStats.sceneBuildMs,
                                     rayTracingStats.traceMs,
                                     rayTracingStats.copyMs);
                    UI::TextDisabled("SWRT Scene: %u instances / %u triangles / %u BVH nodes",
                                     rayTracingStats.instanceCount,
                                     rayTracingStats.triangleCount,
                                     rayTracingStats.bvhNodeCount);
                    UI::TextDisabled("SWRT Shadow: %s / cache %s / %ux%u / interval %u",
                                     rayTracingStats.shadowUpdatedThisFrame ? "updated" : "idle",
                                     rayTracingStats.shadowReusedThisFrame ? "reuse" : "fresh",
                                     rayTracingStats.shadowMapSize,
                                     rayTracingStats.shadowMapSize,
                                     rayTracingStats.shadowUpdateInterval);
                    UI::TextDisabled("SWRT Reflection: %s / cache %s / %ux%u / interval %u / phase %u/%u",
                                     rayTracingStats.reflectionUpdatedThisFrame ? "updated" : "idle",
                                     rayTracingStats.reflectionReusedThisFrame ? "reuse" : "fresh",
                                     rayTracingStats.reflectionWidth,
                                     rayTracingStats.reflectionHeight,
                                     rayTracingStats.reflectionUpdateInterval,
                                     rayTracingStats.reflectionPhaseCount > 0u ? (rayTracingStats.reflectionPhaseIndex + 1u) : 0u,
                                     rayTracingStats.reflectionPhaseCount);
                    UI::TextDisabled("SWRT Reflection Filter: roughness <= %.2f / energy >= %.2f / distance <= %.1f",
                                     rayTracingStats.reflectionMaxRoughness,
                                     rayTracingStats.reflectionMinEnergy,
                                     rayTracingStats.reflectionMaxDistance);
                }
            }

            UI::Separator();
            if (isRayTracingRenderType) {
                int rayTracingPreset = app.GetRayTracingPerformancePresetIndex();
                if (ImGui::Combo("RT Preset", &rayTracingPreset, "Balanced\0Performance\0Ultra Fast\0")) {
                    app.SetRayTracingPerformancePresetIndex(rayTracingPreset);
                }
                int rayTracingBounceCount = app.GetRayTracingMaxBounceCount();
                if (ImGui::SliderInt("RT Bounce Count", &rayTracingBounceCount, 1, 8)) {
                    app.SetRayTracingMaxBounceCount(rayTracingBounceCount);
                }
                UI::TextDisabled("1 = primary only, default = 2");
                bool dynamicResolutionEnabled = app.GetRayTracingDynamicResolutionEnabled();
                if (ImGui::Checkbox("RT Dynamic Resolution", &dynamicResolutionEnabled)) {
                    app.SetRayTracingDynamicResolutionEnabled(dynamicResolutionEnabled);
                }

                const auto rayTracingStats = app.GetRayTracingStats();
                UI::TextDisabled("HWRT Support: %s", app.IsHardwareRayTracingSupported() ? "Yes" : "No");
                UI::TextDisabled("RT Backend: %s",
                                 rayTracingStats.usingHardwarePath ? "Hardware"
                                 : "Unavailable");
                UI::TextDisabled("RT Scene: %u instances / %u triangles",
                                 rayTracingStats.instanceCount,
                                 rayTracingStats.triangleCount);
                UI::TextDisabled("RT BVH Nodes: %u", rayTracingStats.bvhNodeCount);
                UI::TextDisabled("RT Internal: %ux%u (%.2fx)",
                                 rayTracingStats.renderWidth,
                                 rayTracingStats.renderHeight,
                                 rayTracingStats.dynamicResolutionScale);
                UI::TextDisabled("RT Quality: %s",
                                 (rayTracingStats.qualityTier == 0u) ? "Full"
                                 : (rayTracingStats.qualityTier == 1u) ? "Fast"
                                 : "UltraFast");
                UI::TextDisabled("RT Cost: total %.2f ms / build %.2f / trace %.2f / copy %.2f",
                                 rayTracingStats.lastFrameMs,
                                 rayTracingStats.sceneBuildMs,
                                 rayTracingStats.traceMs,
                                 rayTracingStats.copyMs);
                UI::TextDisabled("RT Detail: primary %.2f / shadow %.2f / shade %.2f / resolve %.2f",
                                 rayTracingStats.primaryTraceMs,
                                 rayTracingStats.shadowTraceMs,
                                 rayTracingStats.shadeMs,
                                 rayTracingStats.resolveMs);
            }

            UI::TextDisabled("Mesh Shader Path: optional DX12 Ultimate path");
            }

            if (UI::Tab commonTab{"Common"}) {
            if (app.GetDeltaTime() > 0.0f) {
                UI::Text("FPS: %.1f", 1.0f / app.GetDeltaTime());
            } else {
                UI::Text("FPS: --");
            }
            }

            if (UI::Tab giTab{"GI"}) {
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
            UI::TextDisabled("Origin: (%.1f, %.1f, %.1f)  Spacing: %.1fm",
                             grid.GetOriginX(), grid.GetOriginY(), grid.GetOriginZ(),
                             grid.GetSpacingX());
            }

            if (UI::Tab particleTab{"Particle"}) {
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

SASAMI_IMPLEMENT_APPLICATION(SasamiRenderer::RenderingApp, 1280, 720, L"PBR App")
