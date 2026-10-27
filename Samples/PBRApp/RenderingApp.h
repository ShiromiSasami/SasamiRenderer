#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include <array>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

// AppFramework layer type: declared in its own namespace so this stays the same entity as the
// definition rather than a new SasamiEngine:: type that happens to share the name.
namespace SasamiEngine::App { class StaticModel; }

namespace SasamiEngine::Sample::PBR
{

    // The sample's implementation is split by responsibility, one translation unit each:
    //   RenderingApp.cpp               the IApplication hooks below and the entry point
    //   RenderingAppScene.cpp          the camera pose, environment map, materials and models
    //   RenderingAppInput.cpp          the InputSystem bindings
    //   RenderingAppSessionState.cpp   the save/restore sequence
    //   RenderingAppSettingsFile.cpp   which fields PBRApp.settings.ini carries
    //   RenderingAppGiProbeCache.cpp   the GI probe cache's fingerprint, write and restore
    //   RenderingAppUI*.cpp            the "PBR Controls" ImGui panel
    // The members below stay member functions rather than free functions because their bodies
    // read and write many RenderingApp members directly, which would otherwise have to be
    // threaded through as extra parameters.
    class RenderingApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        // Scene definition (RenderingAppScene.cpp). Both report the failure and call
        // ApplicationCore::RequestQuit themselves, so false only means "stop here".
        bool CreateSceneCamera(ApplicationCore& app);
        bool BuildSampleScene(ApplicationCore& app);

        // Input wiring (RenderingAppInput.cpp).
        void BindInputEvents(ApplicationCore& app);

        // "PBR Controls" ImGui panel (RenderingAppUI.cpp). The two largest tab bodies get a file
        // each - RenderingAppUILighting.cpp and RenderingAppUIRendering.cpp - so
        // RenderingAppUI.cpp keeps nothing but the panel shell, the overlays and the tab order.
        void RegisterUI(ApplicationCore& app);
        void DrawLightingTab(ApplicationCore& app);
        void DrawRenderingTab(ApplicationCore& app);

        // Session save/restore sequencing (RenderingAppSessionState.cpp). Camera and lights go
        // through ApplicationCore SaveScene/ApplyCameraAndLights; the rest through the two
        // stores below.
        void SaveSessionState(ApplicationCore& app) const;
        void LoadSessionState(ApplicationCore& app);

        // PBRApp.settings.ini: render settings + GI toggles + app UI settings
        // (RenderingAppSettingsFile.cpp).
        void WriteSettingsFile(ApplicationCore& app, const std::string& path) const;
        void ReadSettingsFile(ApplicationCore& app, const std::string& path);

        // GI probe cache (RenderingAppGiProbeCache.cpp). The probe grid fit and the cache load /
        // bake request depend on final scene bounds and renderer GI/SWRT readiness, so they run
        // only once IsGiSessionRestoreReady holds - after the async Sponza/bunny loads and the
        // renderer's deferred init tasks complete. ApplyDeferredGiSessionState still returns
        // false while Scene Auto has no scene bounds to fit, and the caller retries.
        void PersistGiProbeCache(ApplicationCore& app) const;
        bool IsGiSessionRestoreReady(ApplicationCore& app) const;
        bool ApplyDeferredGiSessionState(ApplicationCore& app);

        Camera* m_camera = nullptr;
        StaticModel* m_sphereModel = nullptr;
        StaticModel* m_boxModel = nullptr;
        StaticModel* m_transparentSphereModel = nullptr;
        StaticModel* m_transparentBoxModel = nullptr;
        StaticModel* m_bunnyModel = nullptr;
        StaticModel* m_sponzaModel = nullptr;
        StaticModel* m_bistroModel = nullptr;
        SurfaceMaterial m_sphereMaterial{};
        SurfaceMaterial m_boxMaterial{};
        SurfaceMaterial m_transparentSphereMaterial{};
        SurfaceMaterial m_transparentBoxMaterial{};
        bool m_showLightGizmo  = true;
        bool m_showLightGizmos = true; // Point / Spot light gizmos
        // 0=Interior, 1=Wide, 2=Very Wide, 3=Scene Auto (see RenderingAppProbeGrid.cpp).
        // Scene Auto by default: the fixed boxes are all Sponza-sized, so with Bistro loaded at
        // x=60 most of the scene fell outside the probe grid and sampled the visibility-ignoring
        // fallback in GI_Common.hlsli instead of real probes.
        int m_probeGridPreset = 3;
        bool m_sessionGiBaked = false;         // "gi/baked" flag stashed by ReadSettingsFile
        bool m_pendingGiSessionRestore = true; // true until ApplyDeferredGiSessionState has run
        // ApplicationCore::GetSceneGeneration at the time the pointers above were taken. A
        // runtime scene.load destroys every object they point at (RenderingApp.cpp).
        uint64_t m_sceneGeneration = 0;
        void ReleaseReplacedSceneObjects(ApplicationCore& app);
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
    };
}
