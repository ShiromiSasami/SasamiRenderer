#include "AppFramework/Debug/RemoteControl/DebugRenderCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Loader/MeshTriangleStats.h"
#include "AppFramework/Loader/MeshVertexCacheOptimizer.h"
#include "AppFramework/Loader/StaticMeshCache.h"
#include "AppFramework/Object/Camera.h"
#include "Renderer/Runtime/Renderer.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // Names mirror RendererEnums::GBufferDebugView (Source/Renderer/Structures/RendererEnums.h).
        // Index order is authoritative there; this table exists only so the command can be driven by
        // name instead of a magic number.
        const char* const kGBufferViewNames[] = {
            "FinalLit", "Albedo", "Normal", "Roughness", "Metallic", "AmbientOcclusion",
            "Shadow", "Emissive", "RuntimeAmbientOcclusionRaw", "RuntimeAmbientOcclusionFiltered",
            "DirectionalLightDirection", "DirectionalLightNdotL", "ReflectionRadiance",
            "ReflectionAlpha", "SwrtReflectionHitDistance", "SwrtReflectionComposite",
            "ShadowCascadeIndex",
        };
        constexpr size_t kGBufferViewCount = sizeof(kGBufferViewNames) / sizeof(kGBufferViewNames[0]);

        // Case-insensitive name lookup; returns -1 when not found.
        int FindGBufferViewByName(const std::string& name)
        {
            const std::string lowerName = ToLower(name);
            for (size_t i = 0; i < kGBufferViewCount; ++i) {
                if (ToLower(kGBufferViewNames[i]) == lowerName) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // "3 (Roughness)" or "7 (out of range)" if unknown.
        std::string GBufferViewLabel(int index)
        {
            if (index < 0 || static_cast<size_t>(index) >= kGBufferViewCount) {
                return std::to_string(index) + " (out of range)";
            }
            return std::to_string(index) + " (" + kGBufferViewNames[index] + ")";
        }
    }

    // Every setter below reports the value it reads back from ApplicationCore rather than the
    // value the caller requested, because a couple of these setters (notably the GBuffer debug
    // view) silently clamp or reject the request depending on unrelated renderer state. Reporting
    // the read-back keeps the remote caller from believing a rejected change actually applied.
    void RegisterRenderCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("render.get", "current render state (gbuffer view, exposure, IBL, AO mode, render path)",
            [&app](const std::vector<std::string>&) -> std::string {
                return "OK gbufferView=" + GBufferViewLabel(app.GetGBufferDebugViewIndex())
                     + " exposure=" + FormatFloat(app.GetExposure())
                     + " iblIntensity=" + FormatFloat(app.GetIblIntensity())
                     + " aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex())
                     // ORed with aoMode on the native path, so aoMode alone does not say whether
                     // the screen-space pass is running.
                     + " runtimeAo=" + (app.GetRuntimeAOEnabled() ? "1" : "0")
                     + " renderPath=" + std::to_string(app.GetRenderPathModeIndex())
                     // Load-time only, so it cannot be set from here -- reported because an A/B
                     // that silently failed to pass the environment variable through would
                     // otherwise look exactly like "the optimization made no difference".
                     + " vertexCacheOpt=" + (IsVertexCacheOptimizationEnabled() ? "1" : "0");
            });

        // Answers "are the shadow pass's triangles smaller than a shadow-map texel?", the last
        // untested explanation for its 241 Mtri/s. Requires SASAMI_TRIANGLE_STATS=1 at launch
        // because the areas are gathered while meshes are loaded.
        registry.Register("render.trianglestats", "world-space triangle area distribution of the loaded static geometry",
            [](const std::vector<std::string>&) -> std::string {
                // Texel footprints for a 4096 shadow map covering roughly 20m / 55m / 219m --
                // the near, middle and far cascade extents at the scene's current settings.
                // A triangle below one of these covers less than a texel in that cascade.
                const std::vector<float> texelAreas = {
                    (20.0f / 4096.0f) * (20.0f / 4096.0f),
                    (55.0f / 4096.0f) * (55.0f / 4096.0f),
                    (219.0f / 4096.0f) * (219.0f / 4096.0f),
                };
                return FormatTriangleAreaReport(texelAreas);
            });

        // Phase 1 of the virtual-geometry plan: DAG build only, nothing renders from it yet.
        registry.Register("render.dagstats", "build (cached) and report the virtual-geometry cluster DAG statistics of the submitted static meshes",
            [&app](const std::vector<std::string>&) -> std::string {
                return app.GetRenderer().GetSceneSubmitter()
                    .EnsureClusterDagsBuilt(StaticMeshCache::GetCacheDirectory() / "dag")
                    .FormatReport();
            });

        // Independent of render.meshshader: this picks the pipeline for the vertex-shader draws
        // (skinned meshes always, static opaque whenever the cluster path is off).
        registry.Register("render.tessellation", "<0|1> - enable the tessellation pipeline for the vertex-shader draws (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK tessellation=") + (renderer.GetUseTessellation() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.tessellation <0|1>";
                }
                renderer.SetUseTessellation(*value != 0);
                return std::string("OK tessellation=") + (renderer.GetUseTessellation() ? "1" : "0");
            });

        registry.Register("render.meshshader", "<0|1> - draw static opaque geometry through the virtual-geometry cluster path (AS LOD selection + mesh shader); no argument: report current",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK meshShader=") + (renderer.GetUseMeshShader() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.meshshader <0|1>";
                }
                renderer.SetUseMeshShader(*value != 0);
                return std::string("OK meshShader=") + (renderer.GetUseMeshShader() ? "1" : "0");
            });

        // Separate from render.lodthreshold: shadow views run the ortho LOD mode, so this budget is
        // in shadow-map texels. Shadows are PCF-filtered and low-frequency, so a coarser cut here buys
        // most of the frame time back without touching the primary view.
        registry.Register("render.clusterdebug", "<0|1|2> - virtual-geometry debug colouring: 0 off, 1 by cluster, 2 by group (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK clusterDebug=" + std::to_string(renderer.GetClusterDebugMode());
                }
                const auto value = ParseInt(args[0]);
                if (!value || *value < 0 || *value > 2) {
                    return "ERR usage: render.clusterdebug <0|1|2>";
                }
                renderer.SetClusterDebugMode(static_cast<uint32_t>(*value));
                return "OK clusterDebug=" + std::to_string(renderer.GetClusterDebugMode());
            });

        registry.Register("render.shadowlodthreshold", "<texels> - target error of the cluster LOD test in the shadow cascades, in shadow-map texels (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK shadowLodThresholdPx=" + FormatFloat(renderer.GetClusterShadowLodThresholdPx());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value < 0.0f) {
                    return "ERR usage: render.shadowlodthreshold <texels >= 0>";
                }
                renderer.SetClusterShadowLodThresholdPx(*value);
                return "OK shadowLodThresholdPx=" + FormatFloat(renderer.GetClusterShadowLodThresholdPx());
            });

        registry.Register("render.clusterselectonly", "<0|1> - diagnostic: run the cluster-select CS for each cascade but skip the indirect draw, so render.gputime's Shadow figure splits into selection vs drawing (no argument: report current). Leaves the shadow map empty while on.",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK clusterShadowSelectOnly=") + (renderer.GetClusterShadowSelectOnly() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.clusterselectonly <0|1>";
                }
                renderer.SetClusterShadowSelectOnly(*value != 0);
                return std::string("OK clusterShadowSelectOnly=") + (renderer.GetClusterShadowSelectOnly() ? "1" : "0");
            });

        registry.Register("render.clusterselectstats", "<0|1> - diagnostic: make the cluster-select CS tally BVH nodes visited, traversal levels and cluster descs examined, reported by render.streamstats as nodes=/levels=/examined= (no argument: report current). Off by default so the steady-state path runs no extra atomics (their measured cost was below run-to-run noise).",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK clusterSelectStats=") + (renderer.GetClusterSelectStats() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.clusterselectstats <0|1>";
                }
                renderer.SetClusterSelectStats(*value != 0);
                return std::string("OK clusterSelectStats=") + (renderer.GetClusterSelectStats() ? "1" : "0");
            });

        registry.Register("render.lodthreshold", "<px> - target screen-space error of the cluster LOD test in pixels (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK lodThresholdPx=" + FormatFloat(renderer.GetClusterLodThresholdPx());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value < 0.0f) {
                    return "ERR usage: render.lodthreshold <px >= 0>";
                }
                renderer.SetClusterLodThresholdPx(*value);
                return "OK lodThresholdPx=" + FormatFloat(renderer.GetClusterLodThresholdPx());
            });

        registry.Register("render.particles", "<0|1> - GPU particle rendering (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return std::string("OK particles=") + (app.GetParticlesEnabled() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.particles <0|1>";
                }
                app.SetParticlesEnabled(*value != 0);
                return std::string("OK particles=") + (app.GetParticlesEnabled() ? "1" : "0");
            });

        registry.Register("smoke", "<0|1> - volumetric smoke simulation and raymarch (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK smoke=") + (renderer.GetSmokeEnabled() ? "1" : "0") +
                           " jacobi=" + std::to_string(renderer.GetSmokeJacobiIterations());
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: smoke <0|1>";
                }
                renderer.SetSmokeEnabled(*value != 0);
                return std::string("OK smoke=") + (renderer.GetSmokeEnabled() ? "1" : "0");
            });

        registry.Register("smoke.wind", "<x> <y> <z> <strength> - ambient wind pushing the smoke (strength 0 disables)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK windStrength=" + std::to_string(renderer.GetSmokeWindStrength());
                }
                if (args.size() < 4u) {
                    return "ERR usage: smoke.wind <x> <y> <z> <strength>";
                }
                const auto x = ParseFloat(args[0]);
                const auto y = ParseFloat(args[1]);
                const auto z = ParseFloat(args[2]);
                const auto strength = ParseFloat(args[3]);
                if (!x || !y || !z || !strength || *strength < 0.0f) {
                    return "ERR usage: smoke.wind <x> <y> <z> <strength >= 0>";
                }
                renderer.SetSmokeWind(*x, *y, *z, *strength);
                return "OK windStrength=" + std::to_string(renderer.GetSmokeWindStrength());
            });

        registry.Register("smoke.vorticity", "<strength> - vorticity confinement (0 disables; advection dissipates swirl without it)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK vorticity=" + std::to_string(renderer.GetSmokeVorticity());
                }
                const auto strength = ParseFloat(args[0]);
                if (!strength || *strength < 0.0f) {
                    return "ERR usage: smoke.vorticity <strength >= 0>";
                }
                renderer.SetSmokeVorticity(*strength);
                return "OK vorticity=" + std::to_string(renderer.GetSmokeVorticity());
            });

        registry.Register("smoke.buoyancymode", "<0|1> - buoyancy driver: 0 density (legacy), 1 temperature (Fedkiw 2001 Boussinesq)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK buoyancyMode=" + std::to_string(static_cast<uint32_t>(renderer.GetSmokeBuoyancyMode()));
                }
                const auto mode = ParseInt(args[0]);
                if (!mode || (*mode != 0 && *mode != 1)) {
                    return "ERR usage: smoke.buoyancymode <0|1>";
                }
                renderer.SetSmokeBuoyancyMode(static_cast<SmokeVolumeSim::BuoyancyMode>(*mode));
                return "OK buoyancyMode=" + std::to_string(static_cast<uint32_t>(renderer.GetSmokeBuoyancyMode()));
            });

        registry.Register("smoke.temperature", "<beta> <alpha> <emitterTemperature> <coolingRate> - temperature-mode buoyancy coefficients",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                const auto report = [&renderer]() {
                    const SmokeVolumeSim& sim = renderer.GetSmokeSim();
                    return "OK beta=" + std::to_string(sim.GetTemperatureBuoyancy()) +
                        " alpha=" + std::to_string(sim.GetDensityWeight()) +
                        " emitterT=" + std::to_string(sim.GetEmitterTemperature()) +
                        " cooling=" + std::to_string(sim.GetCoolingRate());
                };
                if (args.empty()) {
                    return report();
                }
                if (args.size() < 4u) {
                    return "ERR usage: smoke.temperature <beta> <alpha> <emitterT> <cooling> (all >= 0)";
                }
                const auto beta = ParseFloat(args[0]);
                const auto alpha = ParseFloat(args[1]);
                const auto emitterT = ParseFloat(args[2]);
                const auto cooling = ParseFloat(args[3]);
                if (!beta || !alpha || !emitterT || !cooling ||
                    *beta < 0.0f || *alpha < 0.0f || *emitterT < 0.0f || *cooling < 0.0f) {
                    return "ERR usage: smoke.temperature <beta> <alpha> <emitterT> <cooling> (all >= 0)";
                }
                renderer.SetSmokeTemperatureParams(*beta, *alpha, *emitterT, *cooling);
                return report();
            });

        registry.Register("smoke.emitter", "<x> <y> <z> [radius] - move the smoke source (world space)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() < 3u) {
                    return "ERR usage: smoke.emitter <x> <y> <z> [radius]";
                }
                const auto x = ParseFloat(args[0]);
                const auto y = ParseFloat(args[1]);
                const auto z = ParseFloat(args[2]);
                if (!x || !y || !z) {
                    return "ERR usage: smoke.emitter <x> <y> <z> [radius]";
                }
                float radius = 0.25f;
                if (args.size() >= 4u) {
                    const auto parsed = ParseFloat(args[3]);
                    if (!parsed || *parsed <= 0.0f) {
                        return "ERR usage: smoke.emitter <x> <y> <z> [radius > 0]";
                    }
                    radius = *parsed;
                }
                app.GetRenderer().SetSmokeEmitter(*x, *y, *z, radius);
                return "OK emitter x=" + std::to_string(*x) + " y=" + std::to_string(*y) +
                       " z=" + std::to_string(*z) + " radius=" + std::to_string(radius);
            });

        registry.Register("smoke.volume", "<originX> <originY> <originZ> <cellSize> - place and scale the simulation box",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() < 4u) {
                    return "ERR usage: smoke.volume <originX> <originY> <originZ> <cellSize>";
                }
                const auto x = ParseFloat(args[0]);
                const auto y = ParseFloat(args[1]);
                const auto z = ParseFloat(args[2]);
                const auto cell = ParseFloat(args[3]);
                if (!x || !y || !z || !cell || *cell <= 0.0f) {
                    return "ERR usage: smoke.volume <originX> <originY> <originZ> <cellSize > 0>";
                }
                Renderer& renderer = app.GetRenderer();
                renderer.SetSmokeVolumeOrigin(*x, *y, *z);
                renderer.SetSmokeCellSize(*cell);
                return "OK volume origin=(" + std::to_string(*x) + "," + std::to_string(*y) + "," +
                       std::to_string(*z) + ") cellSize=" + std::to_string(*cell);
            });

        registry.Register("fluid.refraction", "<strength> - screen-space refraction offset of the water surface (0 disables the distortion)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK refraction=" + std::to_string(renderer.GetFluidRefractionStrength());
                }
                const auto strength = ParseFloat(args[0]);
                if (!strength || *strength < 0.0f) {
                    return "ERR usage: fluid.refraction <strength >= 0>";
                }
                renderer.SetFluidRefractionStrength(*strength);
                return "OK refraction=" + std::to_string(renderer.GetFluidRefractionStrength());
            });

        registry.Register("fluid.splash", "<x> <z> [radius] [strength] - inject a splash into the heightfield sim",
            [&app](const std::vector<std::string>& args) -> std::string {
                // The sample's own splash control is an ImGui button, which cannot be driven from
                // this pipe -- and a calm surface grows no foam, so verifying the foam shading
                // needs a way to disturb the water from outside the window.
                if (args.size() < 2u) {
                    return "ERR usage: fluid.splash <x> <z> [radius] [strength]";
                }
                const auto x = ParseFloat(args[0]);
                const auto z = ParseFloat(args[1]);
                if (!x || !z) {
                    return "ERR usage: fluid.splash <x> <z> [radius] [strength]";
                }
                float radius = 0.5f;
                float strength = 1.0f;
                if (args.size() >= 3u) {
                    const auto parsed = ParseFloat(args[2]);
                    if (!parsed || *parsed <= 0.0f) {
                        return "ERR usage: fluid.splash <x> <z> [radius > 0] [strength]";
                    }
                    radius = *parsed;
                }
                if (args.size() >= 4u) {
                    const auto parsed = ParseFloat(args[3]);
                    if (!parsed) {
                        return "ERR usage: fluid.splash <x> <z> [radius] [strength]";
                    }
                    strength = *parsed;
                }
                app.InjectFluidSplash(*x, *z, radius, strength);
                return "OK splash x=" + std::to_string(*x) + " z=" + std::to_string(*z) +
                       " radius=" + std::to_string(radius) + " strength=" + std::to_string(strength);
            });

        registry.Register("render.lensflare.mode", "<0|1|2> - lens flare method: 0 authored sprite table, 1 Silicon Studio procedural (CEDEC 2019), 2 ray traced (Capcom RE:2023)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK lensFlareMode=" + std::to_string(renderer.GetLensFlareMode());
                }
                const auto value = ParseInt(args[0]);
                if (!value || *value < 0 || *value > 2) {
                    return "ERR usage: render.lensflare.mode <0|1|2>";
                }
                renderer.SetLensFlareMode(*value);
                return "OK lensFlareMode=" + std::to_string(renderer.GetLensFlareMode());
            });

        registry.Register("render.lensflare", "<0|1> [intensity] [blades] - ghost lens flare (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                const auto report = [&renderer]() {
                    return std::string("OK lensFlare=") + (renderer.GetLensFlareEnabled() ? "1" : "0") +
                           " intensity=" + std::to_string(renderer.GetLensFlareIntensity()) +
                           " blades=" + std::to_string(renderer.GetLensFlareBladeCount());
                };
                if (args.empty()) {
                    return report();
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.lensflare <0|1> [intensity] [blades]";
                }
                renderer.SetLensFlareEnabled(*value != 0);
                if (args.size() >= 2u) {
                    const auto intensity = ParseFloat(args[1]);
                    if (!intensity || *intensity < 0.0f) {
                        return "ERR usage: render.lensflare <0|1> [intensity >= 0] [blades]";
                    }
                    renderer.SetLensFlareIntensity(*intensity);
                }
                if (args.size() >= 3u) {
                    const auto blades = ParseInt(args[2]);
                    if (!blades || *blades < 3) {
                        return "ERR usage: render.lensflare <0|1> [intensity] [blades >= 3]";
                    }
                    renderer.SetLensFlareBladeCount(*blades);
                }
                return report();
            });

        registry.Register("render.lightshaft", "<0|1> [intensity] - god rays (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                const auto report = [&renderer]() {
                    return std::string("OK lightShaft=") + (renderer.GetLightShaftEnabled() ? "1" : "0") +
                           " intensity=" + std::to_string(renderer.GetLightShaftIntensity());
                };
                if (args.empty()) {
                    return report();
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.lightshaft <0|1> [intensity]";
                }
                renderer.SetLightShaftEnabled(*value != 0);
                if (args.size() >= 2u) {
                    const auto intensity = ParseFloat(args[1]);
                    if (!intensity || *intensity < 0.0f) {
                        return "ERR usage: render.lightshaft <0|1> [intensity >= 0]";
                    }
                    renderer.SetLightShaftIntensity(*intensity);
                }
                return report();
            });

        registry.Register("render.bloom", "<0|1> [intensity] [radius] - mip-chain bloom before tone mapping (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                const auto report = [&renderer]() {
                    return std::string("OK bloom=") + (renderer.GetBloomEnabled() ? "1" : "0") +
                           " intensity=" + std::to_string(renderer.GetBloomIntensity()) +
                           " radius=" + std::to_string(renderer.GetBloomRadius());
                };
                if (args.empty()) {
                    return report();
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.bloom <0|1> [intensity] [radius]";
                }
                if (args.size() >= 2u) {
                    const auto intensity = ParseFloat(args[1]);
                    if (!intensity || *intensity < 0.0f || *intensity > 1.0f) {
                        return "ERR usage: render.bloom <0|1> [intensity 0..1] [radius]";
                    }
                    renderer.SetBloomIntensity(*intensity);
                }
                if (args.size() >= 3u) {
                    const auto radius = ParseFloat(args[2]);
                    if (!radius || *radius <= 0.0f) {
                        return "ERR usage: render.bloom <0|1> [intensity] [radius > 0]";
                    }
                    renderer.SetBloomRadius(*radius);
                }
                renderer.SetBloomEnabled(*value != 0);
                return report();
            });

        registry.Register("render.nativeshadow", "<0|1> - directional shadow map on the native (non-DX12) mesh path (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK nativeShadow=") + (renderer.GetNativeShadowEnabled() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.nativeshadow <0|1>";
                }
                renderer.SetNativeShadowEnabled(*value != 0);
                return std::string("OK nativeShadow=") + (renderer.GetNativeShadowEnabled() ? "1" : "0");
            });

        registry.Register("render.clustergpucull", "<0|1> - GPU BVH traversal + ExecuteIndirect for the directional-shadow cluster path (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK clusterGpuCull=") + (renderer.GetClusterGpuCulling() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.clustergpucull <0|1>";
                }
                renderer.SetClusterGpuCulling(*value != 0);
                return std::string("OK clusterGpuCull=") + (renderer.GetClusterGpuCulling() ? "1" : "0");
            });

        registry.Register("render.clustergpumain", "<0|1> - GPU-driven (ExecuteIndirect) main-view GBuffer cluster draw; needs render.clustergpucull 1, else the CPU-driven draw (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK clusterGpuMain=") + (renderer.GetClusterGpuMainView() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.clustergpumain <0|1>";
                }
                renderer.SetClusterGpuMainView(*value != 0);
                return std::string("OK clusterGpuMain=") + (renderer.GetClusterGpuMainView() ? "1" : "0");
            });

        registry.Register("render.clusterstats", "last frame's virtual-geometry draw: items considered/drawn and amplification groups dispatched",
            [&app](const std::vector<std::string>&) -> std::string {
                const ClusterDrawStats& stats = app.GetRenderer().GetLastClusterDrawStats();
                const ClusterDrawStats& shadowStats = app.GetRenderer().GetLastClusterShadowStats();
                return "OK itemsConsidered=" + std::to_string(stats.itemsConsidered) +
                       " itemsDrawn=" + std::to_string(stats.itemsDrawn) +
                       " clustersDispatched=" + std::to_string(stats.clustersDispatched) +
                       " shadowItemsDrawn=" + std::to_string(shadowStats.itemsDrawn) +
                       " shadowClustersDispatched=" + std::to_string(shadowStats.clustersDispatched) +
                       " gpuCulling=" + (app.GetRenderer().GetClusterGpuCulling() ? "1" : "0") +
                       " gpuVisibleTotal=" + std::to_string(app.GetRenderer().GetClusterStreamingFeedback().GetStats().visibleClustersTotal);
            });

        registry.Register("render.streamstats", "GPU cluster-selection feedback of the last completed frame: visible clusters per view and the wanted-group set that geometry streaming will request",
            [&app](const std::vector<std::string>&) -> std::string {
                return app.GetRenderer().GetClusterStreamingFeedback().FormatReport() + " | " +
                       app.GetRenderer().GetClusterResidency().FormatReport() + " | " +
                       app.GetRenderer().GetClusterPagePool().FormatReport();
            });

        registry.Register("render.pagestats", "virtual-geometry group pages (streaming unit): count, bytes, min/max/avg page size, section bytes",
            [&app](const std::vector<std::string>&) -> std::string {
                SceneSubmitter& submitter = app.GetRenderer().GetSceneSubmitter();
                submitter.EnsureClusterDagGpuBuilt();
                const ClusterPageStore& pages = submitter.GetClusterPageStore();
                return pages.IsBuilt() ? pages.FormatReport() : std::string("ERR cluster pages not built (no meshes or no device)");
            });

        registry.Register("render.poolstats", "GPU page pool residency: resident bytes, ring upload usage, page copies this frame, free-list fragmentation and why loads were deferred (ring/poolFull/pendingFree/fragmentation)",
            [&app](const std::vector<std::string>& args) -> std::string {
                // "render.poolstats reset" clears the peak-load record first, so a swap can be timed
                // without the boot fill -- the largest and noisiest swap of the run -- masking it.
                if (!args.empty() && args[0] == "reset") {
                    app.GetRenderer().GetClusterPagePool().ResetPeakLoadStats();
                }
                return app.GetRenderer().GetClusterPagePool().FormatReport();
            });

        registry.Register("render.streamretention", "<frames> - keep a virtual-geometry group resident for this many frames after it was last requested (0 = evict purely by age; no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK streamRetentionFrames=" + std::to_string(renderer.GetClusterStreamRetentionFrames());
                }
                const auto value = ParseInt(args[0]);
                if (!value || *value < 0) {
                    return "ERR usage: render.streamretention <frames >= 0>";
                }
                renderer.SetClusterStreamRetentionFrames(static_cast<uint32_t>(*value));
                return "OK streamRetentionFrames=" + std::to_string(renderer.GetClusterStreamRetentionFrames());
            });

        registry.Register("render.streambudget", "<groups> - virtual-geometry residency budget in DAG groups (no argument: report current; 0 = unlimited)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK streamBudgetGroups=" + std::to_string(renderer.GetClusterStreamBudgetGroups());
                }
                const auto value = ParseInt(args[0]);
                if (!value || *value < 0) {
                    return "ERR usage: render.streambudget <groups >= 0>";
                }
                renderer.SetClusterStreamBudgetGroups(static_cast<uint32_t>(*value));
                return "OK streamBudgetGroups=" + std::to_string(renderer.GetClusterStreamBudgetGroups());
            });

        registry.Register("render.gbuffer", "<index|name> - set the GBuffer debug view; 'list' enumerates them",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "ERR usage: render.gbuffer <index|name|list>";
                }
                if (ToLower(args[0]) == "list") {
                    std::string out = "OK";
                    for (size_t i = 0; i < kGBufferViewCount; ++i) {
                        out += " " + std::to_string(i) + "=" + kGBufferViewNames[i];
                    }
                    return out;
                }

                int index = -1;
                bool resolved = false;
                if (const auto parsed = ParseInt(args[0])) {
                    index = *parsed;
                    resolved = true;
                } else {
                    index = FindGBufferViewByName(args[0]);
                    resolved = index >= 0;
                }
                if (!resolved) {
                    return "ERR unknown view (try render.gbuffer list)";
                }
                if (index < 0 || static_cast<size_t>(index) >= kGBufferViewCount) {
                    return "ERR index out of range 0.." + std::to_string(kGBufferViewCount - 1);
                }

                app.SetGBufferDebugViewIndex(index);
                // SetGBufferDebugView silently forces FinalLit when the render path is not
                // Raster, so the read-back may differ from the index just requested.
                return "OK gbufferView=" + GBufferViewLabel(app.GetGBufferDebugViewIndex());
            });

        registry.Register("render.exposure", "<f> - set tone-map exposure (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK exposure=" + FormatFloat(app.GetExposure());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.SetExposure(*value);
                return "OK exposure=" + FormatFloat(app.GetExposure());
            });

        // Default off (RenderSettings::enableHdrOutput); the swap chain format / colour space only
        // change on the next ResizeViewport (resize the window to apply after toggling -- see the
        // comment on ResizeViewport in Renderer.cpp for why). Also silently stays SDR when the
        // attached display does not report DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 support.
        registry.Register("render.hdr", "<0|1> - enable HDR10 (PQ/Rec.2020) swap chain output, DX12 raster path only; takes effect on the next resize (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK hdr=") + (renderer.GetEnableHdrOutput() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.hdr <0|1>";
                }
                renderer.SetEnableHdrOutput(*value != 0);
                return std::string("OK hdr=") + (renderer.GetEnableHdrOutput() ? "1" : "0");
            });

        registry.Register("render.paperwhite", "<nits> - HDR10 paper-white luminance used by the PQ encode (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return "OK paperWhiteNits=" + FormatFloat(renderer.GetHdrPaperWhiteNits());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value <= 0.0f) {
                    return "ERR usage: render.paperwhite <nits > 0>";
                }
                renderer.SetHdrPaperWhiteNits(*value);
                return "OK paperWhiteNits=" + FormatFloat(renderer.GetHdrPaperWhiteNits());
            });

        registry.Register("render.nearclip", "<f> - set camera near clip plane (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Camera* camera = app.GetMainCamera();
                if (!camera) {
                    return "ERR no active camera";
                }
                if (args.empty()) {
                    return "OK nearClip=" + FormatFloat(camera->NearClip());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value <= 0.0f) {
                    return "ERR argument must be a positive number";
                }
                camera->SetClipPlanes(*value, camera->FarClip());
                return "OK nearClip=" + FormatFloat(camera->NearClip());
            });

        registry.Register("render.farclip", "<f> - set camera far clip plane (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Camera* camera = app.GetMainCamera();
                if (!camera) {
                    return "ERR no active camera";
                }
                if (args.empty()) {
                    return "OK farClip=" + FormatFloat(camera->FarClip());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value <= 0.0f) {
                    return "ERR argument must be a positive number";
                }
                camera->SetClipPlanes(camera->NearClip(), *value);
                return "OK farClip=" + FormatFloat(camera->FarClip());
            });

        registry.Register("render.shadowdistance", "<f> - set directional shadow distance (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK shadowDistance=" + FormatFloat(app.GetRenderer().GetShadowDistance());
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value <= 0.0f) {
                    return "ERR argument must be a positive number";
                }
                app.GetRenderer().SetShadowDistance(*value);
                return "OK shadowDistance=" + FormatFloat(app.GetRenderer().GetShadowDistance());
            });

        registry.Register("render.shadowcasters", "<0|1> - draw shadow casters (0: shadow maps are cleared but empty; no argument: report)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    const std::string& arg = args[0];
                    if (arg != "0" && arg != "1" && !ParseOnOff(arg)) {
                        return "ERR argument must be 0 or 1";
                    }
                    app.GetRenderer().SetSkipShadowCasters(arg == "0" || arg == "off");
                }
                return std::string("OK shadowCasters=") + (app.GetRenderer().GetSkipShadowCasters() ? "0" : "1");
            });

        registry.Register("render.ibl", "<f> - set IBL intensity (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK iblIntensity=" + FormatFloat(app.GetIblIntensity());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.SetIblIntensity(*value);
                return "OK iblIntensity=" + FormatFloat(app.GetIblIntensity());
            });

        // Goes through GetRenderer() rather than an ApplicationCore accessor, matching
        // render.shadowdistance above: the value lives on the probe grid, and adding a
        // pass-through pair to ApplicationCore for it would buy nothing.
        //
        // Doubles as the way to tell GI apart from the other indirect terms: DeferredLighting_PS
        // picks the probe branch whenever GI is enabled, so iblIntensity cannot be used to
        // isolate it. Setting this to 0 zeroes g_giIntensity, the final multiplier in
        // GI_SampleProbeGrid (GI_Common.hlsli), and whatever brightness survives is not GI.
        registry.Register("render.gi", "<f> - set GI probe intensity (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK giIntensity=" + FormatFloat(app.GetRenderer().GetGIIntensity());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.GetRenderer().SetGIIntensity(*value);
                return "OK giIntensity=" + FormatFloat(app.GetRenderer().GetGIIntensity());
            });

        registry.Register("render.lightfalloff", "[linear|smooth|physical] - point/spot light distance falloff (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                using Falloff = RendererEnums::PunctualLightFalloff;
                Renderer& renderer = app.GetRenderer();

                const auto falloffName = [](Falloff f) -> const char* {
                    switch (f) {
                        case Falloff::LinearSquared: return "linear";
                        case Falloff::SmoothWindow:  return "smooth";
                        default:                     return "physical";
                    }
                };

                if (args.empty()) {
                    return std::string("OK lightFalloff=") + falloffName(renderer.GetPunctualLightFalloff());
                }

                const std::string name = ToLower(args[0]);
                Falloff falloff = Falloff::InverseSquare;
                if (name == "linear" || name == "0")        { falloff = Falloff::LinearSquared; }
                else if (name == "smooth" || name == "1")   { falloff = Falloff::SmoothWindow; }
                else if (name == "physical" || name == "2") { falloff = Falloff::InverseSquare; }
                else { return "ERR falloff must be linear|smooth|physical (or 0|1|2)"; }
                renderer.SetPunctualLightFalloff(falloff);
                return std::string("OK lightFalloff=") + falloffName(renderer.GetPunctualLightFalloff());
            });

        // One selector per indirect term. Refuses the values that have no implementation
        // instead of accepting them silently -- a knob that reads back the value you set but
        // changes nothing is the exact failure this codebase already hit with render.gi.
        registry.Register("render.indirect", "[specular: none|ssr|swrt|hwrt] [diffuse: none|gi|swrt|hwrt] - indirect lighting sources (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                using Spec = RendererEnums::IndirectSpecularSource;
                using Diff = RendererEnums::IndirectDiffuseSource;
                Renderer& renderer = app.GetRenderer();

                const auto specName = [](Spec s) -> const char* {
                    switch (s) {
                        case Spec::ScreenSpace:        return "ssr";
                        case Spec::SoftwareRayTracing: return "swrt";
                        case Spec::HardwareRayTracing: return "hwrt";
                        default:                       return "none";
                    }
                };
                const auto diffName = [](Diff d) -> const char* {
                    switch (d) {
                        case Diff::ProbeGI:            return "gi";
                        case Diff::SoftwareRayTracing: return "swrt";
                        case Diff::HardwareRayTracing: return "hwrt";
                        default:                       return "none";
                    }
                };
                const auto report = [&]() {
                    return std::string("OK specular=") + specName(renderer.GetIndirectSpecularSource())
                         + " diffuse=" + diffName(renderer.GetIndirectDiffuseSource());
                };

                if (args.empty()) {
                    return report();
                }

                const std::string spec = ToLower(args[0]);
                Spec specular = Spec::None;
                if (spec == "ssr")       { specular = Spec::ScreenSpace; }
                else if (spec == "swrt") { specular = Spec::SoftwareRayTracing; }
                else if (spec == "hwrt") { specular = Spec::HardwareRayTracing; }
                else if (spec != "none") { return "ERR specular must be none|ssr|swrt|hwrt"; }
                if (!Renderer::IsIndirectSpecularSourceAvailable(specular)) {
                    return "ERR hwrt specular is not implemented: hardware ray tracing is currently a "
                           "whole-pipeline mode, use render.path 1 instead";
                }
                renderer.SetIndirectSpecularSource(specular);

                if (args.size() >= 2) {
                    const std::string dif = ToLower(args[1]);
                    Diff diffuse = Diff::None;
                    if (dif == "gi")        { diffuse = Diff::ProbeGI; }
                    else if (dif == "swrt") { diffuse = Diff::SoftwareRayTracing; }
                    else if (dif == "hwrt") { diffuse = Diff::HardwareRayTracing; }
                    else if (dif != "none") { return "ERR diffuse must be none|gi|swrt|hwrt"; }
                    if (!Renderer::IsIndirectDiffuseSourceAvailable(diffuse)) {
                        return "ERR that diffuse source is not implemented: per-pixel ray-traced diffuse "
                               "needs ReSTIR GI (the probe grid is already SWRT-traced), and hwrt is a "
                               "whole-pipeline mode";
                    }
                    renderer.SetIndirectDiffuseSource(diffuse);
                }
                return report();
            });

        // Bake progress has to be readable directly. Judging "the bake finished" by two identical
        // screenshots is wrong: a bake that is not progressing at all produces identical frames too,
        // which is exactly how an earlier measurement mistook a stalled grid for a converged one.
        registry.Register("render.gibake", "[start] - GI bake progress (no argument: report; 'start': reset and rebake)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (!args.empty()) {
                    if (ToLower(args[0]) != "start") {
                        return "ERR usage: render.gibake [start]";
                    }
                    renderer.ResetAndRebakeGI();
                }
                const Renderer::GIBakeStatus status = renderer.GetGIBakeStatus();
                return std::string("OK baked=") + (renderer.IsGIBaked() ? "1" : "0")
                     + " progress=" + FormatFloat(status.progress)
                     + " probes=" + std::to_string(status.completedProbes)
                     + "/" + std::to_string(status.totalProbes)
                     + " perStep=" + std::to_string(status.probesPerStep)
                     + " framesLeft=" + std::to_string(status.estimatedFramesRemaining)
                     + " stalled=" + std::to_string(status.stalledFrames)
                     + " phase=" + std::string(status.currentPhase);
            });

        // Probe spacing drives the trilinear interpolation banding on large flat surfaces, so
        // it needs to be reachable from a script to measure whether tightening it actually
        // reduces the banding. Setting it queues a rebake; the grid is unusable until that
        // finishes (watch the GI tab's bake status).
        registry.Register("render.giprobes", "[spacing] [budget] - probe grid spacing in metres (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                auto& renderer = app.GetRenderer();
                const auto report = [&renderer]() {
                    auto& grid = renderer.GetProbeGrid();
                    const std::uint32_t total = grid.GetTotalProbeCount();
                    // 9 float4 of L2 SH per probe.
                    const double megabytes = static_cast<double>(total) * 144.0 / (1024.0 * 1024.0);
                    return "OK spacing=" + FormatFloat(grid.GetSpacingX())
                         + " grid=" + std::to_string(grid.GetCountX())
                         + "x" + std::to_string(grid.GetCountY())
                         + "x" + std::to_string(grid.GetCountZ())
                         + " probes=" + std::to_string(total)
                         + " probeMB=" + FormatFloat(static_cast<float>(megabytes));
                };
                if (args.empty()) {
                    return report();
                }
                const auto spacing = ParseFloat(args[0]);
                if (!spacing || *spacing <= 0.0f) {
                    return "ERR usage: render.giprobes [spacing>0] [budget]";
                }
                std::uint32_t budget = 16384u;
                if (args.size() >= 2) {
                    const auto parsed = ParseInt(args[1]);
                    if (!parsed || *parsed <= 0) {
                        return "ERR budget must be a positive integer";
                    }
                    budget = static_cast<std::uint32_t>(*parsed);
                }
                renderer.SetProbeGridSpacing(*spacing, budget);
                return report();
            });

        // SWRT reflection has no runtime toggle anywhere else -- it is a RenderSettings field with
        // no UI and no command -- which made the indirect-specular double-count impossible to A/B
        // on real hardware: the bug only shows when reflections are actually running. Enabling it
        // also forces SSR off (Renderer::BuildRenderPassExecutionPolicy), so this one switch picks
        // between the two reflection sources.
        registry.Register("render.swrtreflection", "<0|1> - enable SWRT ray-traced reflections (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                Renderer& renderer = app.GetRenderer();
                if (args.empty()) {
                    return std::string("OK swrtReflection=") +
                           (renderer.GetRasterSoftwareRayTracedReflectionEnabled() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.swrtreflection <0|1>";
                }
                renderer.SetRasterSoftwareRayTracedReflectionEnabled(*value != 0);
                return std::string("OK swrtReflection=") +
                       (renderer.GetRasterSoftwareRayTracedReflectionEnabled() ? "1" : "0");
            });

        registry.Register("render.ao", "<index> - set ambient occlusion mode (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex());
                }
                const auto value = ParseInt(args[0]);
                if (!value) {
                    return "ERR argument must be an integer";
                }
                app.SetAmbientOcclusionModeIndex(*value);
                return "OK aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex());
            });

        // The master switch for every runtime AO path. runtimeAoEnabled defaults to true and had no
        // switch outside the UI, so an A/B over `render.ao` alone could not reach "no runtime AO".
        // It is AND-ed with "aoMode != MaterialOnly" on all four backends (Renderer::
        // GetEffectiveAmbientOcclusionMode), so 0 here is sufficient on its own; it used to be OR-ed
        // on the three native backends, which made `render.runtimeao 0` a no-op there.
        registry.Register("render.runtimeao", "<0|1> - enable the runtime (screen-space) ambient occlusion pass (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return std::string("OK runtimeAo=") + (app.GetRuntimeAOEnabled() ? "1" : "0");
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.runtimeao <0|1>";
                }
                app.SetRuntimeAOEnabled(*value != 0);
                return std::string("OK runtimeAo=") + (app.GetRuntimeAOEnabled() ? "1" : "0");
            });

        // Choosing a sensible default for runtimeAoIntensity means sweeping it and diffing the
        // frames, and that has to happen inside one run: with no switch here, each value needed a
        // relaunch, and a relaunch re-randomises the temporally accumulated passes, so the
        // run-to-run noise swamped the few-LSB difference the sweep is trying to resolve.
        // The value is a linear multiplier on the occlusion ratio in SSAO_PS.hlsl
        // (visibility = saturate(1 - occlusion/weight * intensity)) and doubles as the RTAO power
        // in SWRTExecutor.cpp, so it moves both runtime AO methods.
        registry.Register("render.runtimeao.intensity", "<f> - runtime AO strength multiplier, shared by SSAO and RTAO (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK runtimeAoIntensity=" + FormatFloat(app.GetRuntimeAOIntensity());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.SetRuntimeAOIntensity(*value);
                // Negative input is clamped to 0 by RenderFeatureSettings, hence the read-back.
                return "OK runtimeAoIntensity=" + FormatFloat(app.GetRuntimeAOIntensity());
            });

        // Which generator the runtime AO slot uses. Needed alongside the intensity switch because
        // the two methods share runtimeAoIntensity: without this, whichever method the restored
        // ini happened to select decided what the sweep was actually measuring.
        registry.Register("render.runtimeao.method", "<0=SSAO|1=RayTraced> - runtime AO generator (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK runtimeAoMethod=" + std::to_string(app.GetRuntimeAOMethodIndex());
                }
                const auto value = ParseInt(args[0]);
                if (!value || (*value != 0 && *value != 1)) {
                    return "ERR usage: render.runtimeao.method <0|1>";
                }
                app.SetRuntimeAOMethodIndex(*value);
                return "OK runtimeAoMethod=" + std::to_string(app.GetRuntimeAOMethodIndex());
            });

        registry.Register("render.path", "<0=Raster|1=HardwareRayTracing> - set render path (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK renderPath=" + std::to_string(app.GetRenderPathModeIndex());
                }
                const auto value = ParseInt(args[0]);
                if (!value) {
                    return "ERR argument must be an integer";
                }
                app.SetRenderPathModeIndex(*value);
                return "OK renderPath=" + std::to_string(app.GetRenderPathModeIndex());
            });

        // Reports the GPU cost of each render-graph pass so optimisation targets can be picked
        // from measurements instead of guesses. Results lag the frame being recorded by
        // GpuTimestampProfiler::kFrameLatency frames, so the first few polls after launch are empty.
        registry.Register("render.gputime", "per-pass GPU milliseconds for the last completed frame",
            [&app](const std::vector<std::string>&) -> std::string {
                // Distinguish the two empty cases: a profiler that never initialized will stay
                // empty forever, whereas a ready one is merely still filling its latency window.
                if (!app.GetRenderer().IsGpuTimingReady()) {
                    return "ERR GPU timestamp profiler failed to initialize (timing unavailable)";
                }
                const auto& timings = app.GetRenderer().GetGpuPassTimings();
                if (timings.empty()) {
                    return "PENDING profiler ready but no results yet (scopesThisFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingScopesThisFrame())
                         + " lastResolvedScopes="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastResolvedScopeCount())
                         + " frameCounter="
                         + std::to_string(app.GetRenderer().GetGpuTimingFrameCounter())
                         + " askedFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastUpdateRequestedFrame())
                         + " slotHeldFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastUpdateSlotFrame())
                         + " slotValid="
                         + (app.GetRenderer().GetGpuTimingLastUpdateSlotValid() ? "1" : "0")
                         + ")";
                }

                double total = 0.0;
                std::string out = "OK";
                for (const auto& scope : timings) {
                    total += scope.milliseconds;
                    out += ' ';
                    out += scope.name;
                    out += '=';
                    out += FormatFloat(static_cast<float>(scope.milliseconds));
                }
                out += " | total=" + FormatFloat(static_cast<float>(total));
                return out;
            });
    }
}
