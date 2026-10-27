// RenderingApp's IApplication lifecycle hooks and the sample's entry point. Everything the
// hooks orchestrate lives in its own translation unit (see the map in RenderingApp.h), so this
// file stays a readable sequence: pick the render path, build the scene, wire input and UI,
// restore the previous session, then drive the per-frame work.

#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "ApplicationEntryPoint.h"
#include "Object/StaticModel.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Runtime/Renderer.h"

namespace SasamiEngine::Sample::PBR
{
    void RenderingApp::OnInit(ApplicationCore& app)
    {
        // CreateSceneCamera / BuildSampleScene report and RequestQuit on failure
        // themselves, so a false return only has to abort the rest of the sequence.
        if (!CreateSceneCamera(app)) {
            return;
        }

        const bool useFullDx12RenderGraph = (app.GetGraphicsRuntime() == GraphicsRuntime::DirectX12);
        if (useFullDx12RenderGraph) {
            app.UseDefaultRenderNodePreset();
        } else {
            DebugLog("PBRApp: non-DX12 backend uses native mesh frame path; DX12 render graph shadow/post passes are skipped.\n");
        }

        if (!BuildSampleScene(app)) {
            return;
        }

        BindInputEvents(app);
        RegisterUI(app);
        m_sceneGeneration = app.GetSceneGeneration();

        // Restore camera, lights and settings saved from a previous run (if any).
        // Applied last so it overrides the default view/lights/settings above.
        LoadSessionState(app);
    }

    void RenderingApp::ReleaseReplacedSceneObjects(ApplicationCore& app)
    {
        if (app.GetSceneGeneration() == m_sceneGeneration) {
            return;
        }
        m_sceneGeneration = app.GetSceneGeneration();

        // The scene was cleared and rebuilt (LoadScene): every object pointer below now points at
        // freed memory. The loaded scene brings its own camera; the sample's models are gone, and
        // the null checks in the UI and IsGiSessionRestoreReady already treat that as absent.
        m_camera = app.GetMainCamera();
        m_sphereModel = nullptr;
        m_boxModel = nullptr;
        m_transparentSphereModel = nullptr;
        m_transparentBoxModel = nullptr;
        m_bunnyModel = nullptr;
        m_sponzaModel = nullptr;
        m_bistroModel = nullptr;
        DebugLog("[PBRApp] scene replaced; sample object pointers released\n");
    }

    void RenderingApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        ReleaseReplacedSceneObjects(app);
        if (m_pendingGiSessionRestore && app.IsRendererReady() && IsGiSessionRestoreReady(app)) {
            m_pendingGiSessionRestore = !ApplyDeferredGiSessionState(app);
            if (!m_pendingGiSessionRestore) {
                DebugLog("[PBRApp] deferred GI session state applied\n");
            }
        }

        if (m_camera) {
            m_camera->Update(deltaTime);
        }
    }

    void RenderingApp::OnRender(ApplicationCore& app)
    {
        // Debug-pipe commands (scene.load among them) drain after OnUpdate, and the UI drawn
        // from RenderFrame reads m_camera, so the check is repeated here.
        ReleaseReplacedSceneObjects(app);
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
}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::PBR::RenderingApp, 1280, 720, L"PBR App")
