// The schema of PBRApp.settings.ini: which render settings, GI toggles and app UI toggles are
// persisted, under which section and key. Nothing here decides *when* a save or restore happens
// (RenderingAppSessionState.cpp), how the text is parsed (IniDocument) or how it is formatted
// (IniWriter), so adding a persisted field touches only this file.
//
// The file is sparse: only values that differ from the code's defaults are written, and a key
// that is absent reads back as whatever the default is *today*. That is what lets a changed
// default reach an existing install. The old dense file pinned every field forever instead, so
// e.g. clusterShadowLodThresholdPx = 8 outlived its revert back to 4 on every machine that
// happened to save between 2026-09-25 and 2026-09-27. It also retires the habit of renaming a
// key whenever a default flipped (useMeshShader -> useClusterMeshShader -> clusterMeshShaderPath),
// since the rename only existed to make old files stop being believed.
//
// What sparseness cannot express is an existing key changing *meaning*: a rename, a unit change
// or an enum renumber all leave an old file looking perfectly valid. For those there is
// [meta] version, read before anything else and handed to MigrateSettings below.

#include "RenderingApp.h"
#include "Config/IniDocument.h"
#include "Config/IniWriter.h"
#include "ApplicationCore.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Runtime/Renderer.h"
#include "RenderingAppSessionFiles.h"

// The renderer headers above pull in the Windows headers, whose min / max macros break the
// std::clamp below. Undefined the same way the renderer's own translation units do.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

namespace SasamiEngine::Sample::PBR
{
    using App::IniDocument;
    using App::IniWriter;

    namespace
    {
        // Bumped only when an existing key changes meaning. Adding or retiring a key needs no
        // bump: sparseness already covers both.
        //   1 = dense layout, every field written unconditionally. It had no readable version
        //       key -- its "# SasamiRenderer PBRApp settings v1" banner was a comment, which
        //       IniDocument drops -- so an absent version key is exactly how a v1 file looks.
        //   2 = sparse layout, defaults omitted, version key readable.
        constexpr long kSettingsFileVersion = 2;

        // Rewrites `doc` in place so the reader below can treat it as current. Empty on purpose:
        // v1 -> v2 removed keys and changed nothing's meaning, and every v1 key still parses as
        // what the reader expects. Future rules go here, e.g.
        //   if (fileVersion < 3) doc.RenameKey("render_settings", "oldName", "newName");
        void MigrateSettings(IniDocument& doc, long fileVersion)
        {
            (void)doc;
            (void)fileVersion;
        }

        // Pristine defaults for the two sections that are not backed by an aggregate the way
        // [render_settings] is backed by RenderSettings{}. They are spelled out here only
        // because the owning objects expose no "what would a fresh install hold" accessor;
        // each one mirrors the single declaration named beside it, and reaching into the
        // renderer for them would mean editing that project (see IniDocument.h).
        struct SessionDefaults
        {
            static constexpr bool  giEnabled        = true;   // IrradianceProbeGrid::m_enabled
            static constexpr float giIntensity      = 1.0f;   // IrradianceProbeGrid::m_giIntensity
            static constexpr float giEmaAlpha       = 0.1f;   // IrradianceProbeGrid::m_emaAlpha
            static constexpr bool  giContinuousMode = false;  // Renderer::m_giContinuousMode
            // Not a setting but a session note: "a bake was saved alongside this file".
            static constexpr bool  giBaked          = false;

            static constexpr int   probeGridPreset  = 3;      // RenderingApp::m_probeGridPreset
            static constexpr bool  showLightGizmo   = true;   // RenderingApp::m_showLightGizmo
            static constexpr bool  showLightGizmos  = true;   // RenderingApp::m_showLightGizmos
            static constexpr bool  showProbeSpheres = false;  // DebugProbeGridRenderPass::m_enabled
            static constexpr float probeRadius      = 0.2f;   // DebugProbeGridRenderPass::m_probeRadius
            static constexpr bool  showParticles    = false;  // ParticleRenderPass::m_enabled
        };
    }

    void RenderingApp::WriteSettingsFile(ApplicationCore& app, const std::string& path) const
    {
        std::ofstream out(path);
        if (!out.is_open()) {
            DebugLog("PBRApp: failed to open settings state file for writing.\n");
            return;
        }

        Renderer& renderer = app.GetRenderer();
        const RenderSettings& s = renderer.GetRenderSettings();
        // The yardstick for every comparison below: a field that still matches this is one the
        // user never touched, and leaving it out of the file is what keeps it following the code.
        const RenderSettings d{};

        IniWriter w(out);

        out << "# SasamiRenderer PBRApp settings. Only values that differ from the built-in\n";
        out << "# defaults are stored; delete a line to go back to the default, delete the file\n";
        out << "# to reset everything.\n";

        w.BeginSection("meta");
        w.WriteAlwaysInt("version", kSettingsFileVersion);

        w.BeginSection("render_settings");
#define WRITE_F(field) w.WriteFloat(#field, s.field, d.field)
#define WRITE_U(field) w.WriteInt(#field, static_cast<long>(s.field), static_cast<long>(d.field))
#define WRITE_B(field) w.WriteBool(#field, s.field, d.field)
#define WRITE_E(field) w.WriteInt(#field, static_cast<long>(s.field), static_cast<long>(d.field))
        WRITE_F(iblIntensity);
        WRITE_B(useTessellation);
        WRITE_B(tessWireframeEnabled);
        WRITE_B(tessDebugColorsEnabled);
        // Still under its own key rather than "useMeshShader": ini files from the era when that
        // flag was inert carry "useMeshShader = 1" meaning "standard VS", and reading it back
        // would switch those installs onto the cluster path. Nothing new should need this trick
        // now that a default change propagates on its own.
        w.WriteBool("clusterMeshShaderPath", s.useMeshShader, d.useMeshShader);
        // Both cluster LOD budgets. Different units: the primary view is in screen pixels, the
        // shadow cascades in shadow-map texels.
        WRITE_F(clusterLodThresholdPx);
        WRITE_F(clusterShadowLodThresholdPx);
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
        WRITE_E(punctualLightFalloff);
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

        w.BeginSection("gi");
        w.WriteBool("enabled",         renderer.GetGIEnabled(),        SessionDefaults::giEnabled);
        w.WriteFloat("intensity",      renderer.GetGIIntensity(),      SessionDefaults::giIntensity);
        w.WriteFloat("ema_alpha",      renderer.GetGIEmaAlpha(),       SessionDefaults::giEmaAlpha);
        w.WriteBool("baked",           renderer.HasGIProbeData(),      SessionDefaults::giBaked);
        w.WriteBool("continuous_mode", renderer.GetGIContinuousMode(), SessionDefaults::giContinuousMode);

        // kGIProbeCacheFile used to be echoed here as "probe_cache". It was a compile-time
        // constant that nothing ever read back, so under sparse rules it is not a setting at all.
        w.BeginSection("app");
        w.WriteInt("probe_preset",        m_probeGridPreset,                 SessionDefaults::probeGridPreset);
        w.WriteBool("show_probe_spheres", app.GetDebugProbeGridEnabled(),    SessionDefaults::showProbeSpheres);
        w.WriteFloat("probe_radius",      app.GetDebugProbeRadius(),         SessionDefaults::probeRadius);
        w.WriteBool("show_light_gizmo",   m_showLightGizmo,                  SessionDefaults::showLightGizmo);
        w.WriteBool("show_light_gizmos",  m_showLightGizmos,                 SessionDefaults::showLightGizmos);
        w.WriteBool("show_particles",     app.GetParticlesEnabled(),         SessionDefaults::showParticles);

        // Closed before returning: the caller fingerprints this file on disk straight after.
        out.close();
    }

    void RenderingApp::ReadSettingsFile(ApplicationCore& app, const std::string& path)
    {
        m_sessionGiBaked = false;

        IniDocument doc;
        if (!doc.LoadFromFile(path)) {
            // First run, or the file was removed. Deliberately not an early return: an empty
            // document yields every fallback below, which is the full reset that deleting the
            // file is supposed to mean, and makes a mid-session reload behave like a cold start.
            DebugLog("PBRApp: no settings state file; starting from built-in defaults.\n");
        }

        // Absent means a pre-sparse file. Nothing to do for it today, but the hook has to run
        // before a single key is read or a future rule would be reading a stale name.
        MigrateSettings(doc, doc.GetLong("meta", "version", 1));

        Renderer& renderer = app.GetRenderer();
        // Pristine defaults, not renderer.GetRenderSettings(): a key that is absent has to land
        // on the code's current default, not on whatever this session last set. Starting from
        // the live settings would make a reload keep values the file no longer mentions, so
        // deleting a line (or the file) would quietly do nothing.
        RenderSettings s{};
#define READ_F(field) s.field = doc.GetFloat("render_settings", #field, s.field)
        // Negative clamp: the field is unsigned, so a negative in a hand-edited file would wrap
        // to billions of bounces / samples rather than fail.
#define READ_U(field) s.field = static_cast<uint32_t>(std::max(0L, \
            doc.GetLong("render_settings", #field, static_cast<long>(s.field))))
#define READ_B(field) s.field = doc.GetBool("render_settings", #field, s.field)
        // Range-checked the way ApplicationCoreScene.cpp clamps its own enums: an out-of-range
        // integer in the file must not become an enum value that no switch handles. `last` is
        // the highest valid enumerator, named rather than spelled as a literal so adding a
        // value to the enum keeps this honest.
#define READ_E(field, last) s.field = static_cast<decltype(s.field)>(std::clamp( \
            doc.GetLong("render_settings", #field, static_cast<long>(s.field)), \
            0L, static_cast<long>(last)))
        READ_F(iblIntensity);
        READ_B(useTessellation);
        READ_B(tessWireframeEnabled);
        READ_B(tessDebugColorsEnabled);
        // See the matching note in WriteSettingsFile: the key is not the field name on purpose.
        s.useMeshShader = doc.GetBool("render_settings", "clusterMeshShaderPath", s.useMeshShader);
        READ_F(clusterLodThresholdPx);
        READ_F(clusterShadowLodThresholdPx);
        READ_E(renderPathMode, RenderPathMode::HardwareRayTracing);
        READ_E(rayTracingPerformancePreset, RayTracingPerformancePreset::UltraFast);
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
        READ_E(ambientOcclusionMode, AmbientOcclusionMode::Hybrid);
        READ_E(runtimeAoMethod, RuntimeAmbientOcclusionMethod::RayTraced);
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
        READ_E(punctualLightFalloff, PunctualLightFalloff::InverseSquare);
        READ_F(runtimeAoThickness);
        READ_U(runtimeAoQuality);
        READ_U(swrtAoSampleCount);
        READ_E(gBufferDebugView, static_cast<long>(GBufferDebugView::Count) - 1);
        READ_F(hardwareRayTracingResolutionScale);
        READ_B(vsmBlurEnabled);
        READ_F(exposure);
#undef READ_F
#undef READ_U
#undef READ_B
#undef READ_E
        renderer.SetRenderSettings(s);

        renderer.SetGIEnabled(doc.GetBool("gi", "enabled", SessionDefaults::giEnabled));
        renderer.SetGIIntensity(doc.GetFloat("gi", "intensity", SessionDefaults::giIntensity));
        renderer.SetGIEmaAlpha(doc.GetFloat("gi", "ema_alpha", SessionDefaults::giEmaAlpha));
        renderer.SetGIContinuousMode(doc.GetBool("gi", "continuous_mode", SessionDefaults::giContinuousMode));
        m_sessionGiBaked = doc.GetBool("gi", "baked", SessionDefaults::giBaked);

        m_probeGridPreset = std::clamp(
            static_cast<int>(doc.GetLong("app", "probe_preset", SessionDefaults::probeGridPreset)), 0, 3);
        app.SetDebugProbeGridEnabled(doc.GetBool("app", "show_probe_spheres", SessionDefaults::showProbeSpheres));
        app.SetDebugProbeRadius(doc.GetFloat("app", "probe_radius", SessionDefaults::probeRadius));
        m_showLightGizmo  = doc.GetBool("app", "show_light_gizmo", SessionDefaults::showLightGizmo);
        m_showLightGizmos = doc.GetBool("app", "show_light_gizmos", SessionDefaults::showLightGizmos);
        app.SetParticlesEnabled(doc.GetBool("app", "show_particles", SessionDefaults::showParticles));
    }
}
