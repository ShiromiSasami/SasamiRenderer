// The "Rendering" tab was the largest single tab body in RegisterUI's ImGui panel, so it was
// split out of RenderingAppUI.cpp into this file to keep every UI file under the
// single-responsibility line-count threshold. RenderingAppUI.cpp still owns the UI::Tab that
// invokes DrawRenderingTab, so the tab order stays readable from one place. The render-pass
// builder helpers below are only used by this tab, so they moved along with it.

#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "UI/UIButton.h"
#include "UI/UIGate.h"
#include "UI/UIIntVolume.h"
#include "UI/UILayout.h"
#include "UI/UIText.h"
#include "UI/UIVolume.h"
#include "Renderer/Config/BackendFeatureSupport.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/Runtime/Renderer.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <vector>

namespace SasamiEngine::Sample::PBR
{
    namespace
    {
        struct RenderPassBuilderUIEntry
        {
            const char* label = "";
            RendererEnums::RenderPassType type = RendererEnums::RenderPassType::OpaqueGBuffer;
            // Shown but not toggleable: deferred lighting reads this pass's targets, so
            // dropping it renders from targets nothing wrote. Renderer::SetRenderPassSequence
            // restores it regardless; listing it here keeps the UI honest about the graph.
            bool required = false;
            RenderFeature feature = RenderFeature::RenderGraph;
        };

        constexpr std::array<RenderPassBuilderUIEntry, 15> kRenderPassBuilderUIEntries = {{
            { "Shadow", RendererEnums::RenderPassType::Shadow, false, RenderFeature::RenderGraph },
            { "Opaque GBuffer", RendererEnums::RenderPassType::OpaqueGBuffer, true, RenderFeature::RenderGraph },
            { "Runtime AO", RendererEnums::RenderPassType::RuntimeAO, false, RenderFeature::AmbientOcclusion },
            { "Runtime AO Blur", RendererEnums::RenderPassType::RuntimeAOBlur, false, RenderFeature::AmbientOcclusion },
            { "Lighting", RendererEnums::RenderPassType::Lighting, false, RenderFeature::RenderGraph },
            { "Screen Space Reflection", RendererEnums::RenderPassType::ScreenSpaceReflection, false, RenderFeature::ScreenSpaceReflections },
            { "Software Reflection", RendererEnums::RenderPassType::SoftwareReflection, false, RenderFeature::SoftwareRayTracing },
            { "Software Reflection Composite", RendererEnums::RenderPassType::SoftwareReflectionComposite, false, RenderFeature::SoftwareRayTracing },
            { "Skybox", RendererEnums::RenderPassType::Skybox, false, RenderFeature::RenderGraph },
            { "Procedural Sky", RendererEnums::RenderPassType::ProceduralSky, false, RenderFeature::RenderGraph },
            { "Transparent Backface Distance", RendererEnums::RenderPassType::TransparentBackfaceDistance, false, RenderFeature::RenderGraph },
            { "Transparent Scene Color Copy", RendererEnums::RenderPassType::TransparentSceneColorCopy, false, RenderFeature::RenderGraph },
            { "Transparent Lighting", RendererEnums::RenderPassType::TransparentLighting, false, RenderFeature::RenderGraph },
            { "Transparent Composite", RendererEnums::RenderPassType::TransparentComposite, false, RenderFeature::RenderGraph },
            { "Post Process", RendererEnums::RenderPassType::PostProcess, false, RenderFeature::PostProcess },
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

            const auto& features = app.GetFeatureSupport();

            std::vector<ApplicationCore::RenderPassType> currentSequence = app.GetRenderPassSequence();
            std::vector<ApplicationCore::RenderPassType> nextSequence;
            nextSequence.reserve(kRenderPassBuilderUIEntries.size());
            bool changed = false;

            for (const RenderPassBuilderUIEntry& entry : kRenderPassBuilderUIEntries) {
                bool enabled = HasRenderPass(currentSequence, entry.type);
                if (entry.required) {
                    bool alwaysOn = true;
                    {
                        UI::Gate gate{features.IsSupported(entry.feature), features.UnsupportedReason(entry.feature)};
                        ImGui::BeginDisabled();
                        ImGui::Checkbox(entry.label, &alwaysOn);
                        ImGui::EndDisabled();
                    }
                    ImGui::SameLine();
                    UI::TextDisabled("(required by deferred lighting)");
                    nextSequence.push_back(entry.type);
                    continue;
                }
                {
                    UI::Gate gate{features.IsSupported(entry.feature), features.UnsupportedReason(entry.feature)};
                    if (ImGui::Checkbox(entry.label, &enabled)) {
                        changed = true;
                    }
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
    }

    void RenderingApp::DrawRenderingTab(ApplicationCore& app)
    {
        const auto& features = app.GetFeatureSupport();

        int renderPathMode = app.GetRenderPathModeIndex();
        const bool hardwareRtSelected =
            (renderPathMode == static_cast<int>(RendererEnums::RenderPathMode::HardwareRayTracing));
        const char* renderTypePreview = hardwareRtSelected ? "Hardware RT" : "Raster";
        if (ImGui::BeginCombo("Render Type", renderTypePreview)) {
            if (ImGui::Selectable("Raster", !hardwareRtSelected)) {
                renderPathMode = static_cast<int>(RendererEnums::RenderPathMode::Raster);
                app.SetRenderPathModeIndex(renderPathMode);
            }
            {
                UI::Gate gate{features.IsSupported(RenderFeature::HardwareRayTracing),
                              features.UnsupportedReason(RenderFeature::HardwareRayTracing)};
                if (ImGui::Selectable("Hardware RT", hardwareRtSelected)) {
                    renderPathMode = static_cast<int>(RendererEnums::RenderPathMode::HardwareRayTracing);
                    app.SetRenderPathModeIndex(renderPathMode);
                }
            }
            ImGui::EndCombo();
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
            // Two independent axes, not one mode list. Virtual geometry replaces the CPU-issued
            // per-item draw of static opaque meshes with GPU cluster selection; the tessellation
            // toggle below picks which pipeline the remaining vertex-shader draws use (skinned
            // meshes, and static opaque too whenever virtual geometry is off).
            {
                UI::Gate gate{features.IsSupported(RenderFeature::VirtualGeometry),
                              features.UnsupportedReason(RenderFeature::VirtualGeometry)};
                if (ImGui::Checkbox("Virtual geometry (AS/MS clusters)", &useMeshShader)) {
                    app.SetUseMeshShader(useMeshShader);
                }
                ImGui::SameLine();
                UI::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("GPU-driven cluster path for static opaque geometry.\n"
                                      "Falls back to the vertex-shader path automatically when the\n"
                                      "hardware has no mesh shader support.");
                }
                if (useMeshShader) {
                    UI::TextDisabled("Cluster LOD threshold: render.lodthreshold <px> (CLI)");
                }
            }

            {
                UI::Gate gate{features.IsSupported(RenderFeature::Tessellation),
                              features.UnsupportedReason(RenderFeature::Tessellation)};
                UI::TextDisabled("Vertex shader path");
                if (ImGui::RadioButton("Standard (VS only)", !useTessellation)) {
                    app.SetUseTessellation(false);
                }
                if (ImGui::RadioButton("Tessellation (VS+HS+DS+GS)", useTessellation)) {
                    app.SetUseTessellation(true);
                }

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
            }

            int debugMode = app.GetGBufferDebugViewIndex();
            if (ImGui::Combo("GBuffer Debug", &debugMode,
                             "Final Lit\0Albedo\0Normal\0Roughness\0Metallic\0Ambient Occlusion\0Shadow\0Emissive\0Runtime AO Raw\0Runtime AO Filtered\0Directional Light Dir\0Directional NdotL\0Reflection Radiance\0Reflection Alpha\0SWRT Reflection Hit Distance\0SWRT Reflection Composite\0")) {
                app.SetGBufferDebugViewIndex(debugMode);
            }
            UI::TextDisabled("Shortcut: F2 (cycle)");

            bool softwareDirectionalShadow = app.GetRasterSoftwareRayTracedDirectionalShadowEnabled();
            {
                UI::Gate gate{features.IsSupported(RenderFeature::SoftwareRayTracing),
                              features.UnsupportedReason(RenderFeature::SoftwareRayTracing)};
                if (ImGui::Checkbox("SWRT Directional Shadow", &softwareDirectionalShadow)) {
                    app.SetRasterSoftwareRayTracedDirectionalShadowEnabled(softwareDirectionalShadow);
                }
            }
            // Indirect lighting has two independent terms and one selector each. Options with
            // no implementation are listed but disabled with the reason, rather than being
            // selectable and silently doing nothing.
            {
                using Spec = RendererEnums::IndirectSpecularSource;
                using Diff = RendererEnums::IndirectDiffuseSource;
                auto& renderer = app.GetRenderer();

                struct SpecOption { Spec value; const char* label; const char* disabledReason; };
                static const SpecOption kSpecOptions[] = {
                    { Spec::None,               "None",                nullptr },
                    { Spec::ScreenSpace,        "Screen Space (SSR)",  nullptr },
                    { Spec::SoftwareRayTracing, "Software RT (SWRT)",  nullptr },
                    { Spec::HardwareRayTracing, "Hardware RT (HWRT)",
                      "Not a per-term source yet: HWRT replaces the whole pass list and writes all of SceneColor.\nUse Render Path above to switch the entire pipeline to it." },
                };
                const Spec currentSpec = renderer.GetIndirectSpecularSource();
                const char* currentSpecLabel = "None";
                for (const SpecOption& o : kSpecOptions) {
                    if (o.value == currentSpec) { currentSpecLabel = o.label; }
                }
                if (ImGui::BeginCombo("Indirect Specular", currentSpecLabel)) {
                    for (const SpecOption& o : kSpecOptions) {
                        const bool unavailable = !Renderer::IsIndirectSpecularSourceAvailable(o.value);
                        ImGui::BeginDisabled(unavailable);
                        if (ImGui::Selectable(o.label, o.value == currentSpec)) {
                            renderer.SetIndirectSpecularSource(o.value);
                        }
                        ImGui::EndDisabled();
                        if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("%s", o.disabledReason);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Which technique supplies indirect specular. Exactly one at a time.\nSSR ray-marches the depth buffer only (misses dropped, no IBL fallback).");

                struct DiffOption { Diff value; const char* label; const char* disabledReason; };
                static const DiffOption kDiffOptions[] = {
                    { Diff::None,               "None",               nullptr },
                    { Diff::ProbeGI,            "GI Probes",          nullptr },
                    { Diff::SoftwareRayTracing, "Software RT (SWRT)",
                      "Per-pixel ray-traced diffuse needs ReSTIR GI, which is not implemented.\nNote the GI probes above are already traced by SWRT." },
                    { Diff::HardwareRayTracing, "Hardware RT (HWRT)",
                      "Not a per-term source yet: HWRT replaces the whole pass list and writes all of SceneColor." },
                };
                const Diff currentDiff = renderer.GetIndirectDiffuseSource();
                const char* currentDiffLabel = "None";
                for (const DiffOption& o : kDiffOptions) {
                    if (o.value == currentDiff) { currentDiffLabel = o.label; }
                }
                if (ImGui::BeginCombo("Indirect Diffuse", currentDiffLabel)) {
                    for (const DiffOption& o : kDiffOptions) {
                        const bool unavailable = !Renderer::IsIndirectDiffuseSourceAvailable(o.value);
                        ImGui::BeginDisabled(unavailable);
                        if (ImGui::Selectable(o.label, o.value == currentDiff)) {
                            renderer.SetIndirectDiffuseSource(o.value);
                        }
                        ImGui::EndDisabled();
                        if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("%s", o.disabledReason);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Which technique supplies indirect diffuse. Independent of the specular selector above:\nthey are different terms, so turning one off does not substitute for the other.");
            }
            const RendererEnums::IndirectSpecularSource specularSource =
                app.GetRenderer().GetIndirectSpecularSource();
            const bool screenSpaceReflections =
                specularSource == RendererEnums::IndirectSpecularSource::ScreenSpace;
            const bool softwareReflections =
                specularSource == RendererEnums::IndirectSpecularSource::SoftwareRayTracing;
            if (screenSpaceReflections) {
                UI::Gate gate{features.IsSupported(RenderFeature::ScreenSpaceReflections),
                              features.UnsupportedReason(RenderFeature::ScreenSpaceReflections)};
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
            bool aoUsesRtao = false;
            {
                UI::Gate gate{features.IsSupported(RenderFeature::AmbientOcclusion),
                              features.UnsupportedReason(RenderFeature::AmbientOcclusion)};
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
                aoUsesRtao = aoUsesRuntimeAo && (forceRtaoOnly || runtimeAoMethod == 1);
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
            }
            if (softwareReflections) {
                UI::Gate gate{features.IsSupported(RenderFeature::SoftwareRayTracing),
                              features.UnsupportedReason(RenderFeature::SoftwareRayTracing)};
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
                    if (UI::IntVolume("SWRT A-Trous Iterations", &atrousIterations, 0, 5)) {
                        app.SetSwrtReflectionAtrousIterations(atrousIterations);
                    }
                }
                if (softwareDirectionalShadow || softwareReflections || aoUsesRtao) {
                    {
                        UI::Gate gate{features.IsSupported(RenderFeature::SoftwareRayTracing),
                                      features.UnsupportedReason(RenderFeature::SoftwareRayTracing)};
                        int rayTracingPreset = app.GetRayTracingPerformancePresetIndex();
                        if (ImGui::Combo("SWRT Partial RT Preset", &rayTracingPreset, "Balanced\0Performance\0Ultra Fast\0")) {
                            app.SetRayTracingPerformancePresetIndex(rayTracingPreset);
                        }
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
                {
                    UI::Gate gate{features.IsSupported(RenderFeature::HardwareRayTracing),
                                  features.UnsupportedReason(RenderFeature::HardwareRayTracing)};
                    int rayTracingPreset = app.GetRayTracingPerformancePresetIndex();
                    if (ImGui::Combo("RT Preset", &rayTracingPreset, "Balanced\0Performance\0Ultra Fast\0")) {
                        app.SetRayTracingPerformancePresetIndex(rayTracingPreset);
                    }
                    int rayTracingBounceCount = app.GetRayTracingMaxBounceCount();
                    if (UI::IntVolume("RT Bounce Count", &rayTracingBounceCount, 1, 8)) {
                        app.SetRayTracingMaxBounceCount(rayTracingBounceCount);
                    }
                    UI::TextDisabled("1 = primary only, default = 2");
                    bool dynamicResolutionEnabled = app.GetRayTracingDynamicResolutionEnabled();
                    if (ImGui::Checkbox("RT Dynamic Resolution", &dynamicResolutionEnabled)) {
                        app.SetRayTracingDynamicResolutionEnabled(dynamicResolutionEnabled);
                    }
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
    }
}
