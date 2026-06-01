#include "Renderer/Core/SceneSynchronizer.h"

#include <cstring>

namespace SasamiRenderer
{
    SceneSynchronizer::SceneSynchronizer(SceneSubmitter& sceneSubmitter,
                                         CameraState& cameraState,
                                         RayTracingScene& rayTracingScene,
                                         SWRTExecutor& swrtExecutor,
                                         DrawCommandBuilder& drawCommandBuilder,
                                         const float& deltaTime,
                                         const Viewport& viewport)
        : m_sceneSubmitter(sceneSubmitter)
        , m_cameraState(cameraState)
        , m_rayTracingScene(rayTracingScene)
        , m_swrtExecutor(swrtExecutor)
        , m_drawCommandBuilder(drawCommandBuilder)
        , m_deltaTime(deltaTime)
        , m_viewport(viewport)
    {
    }

    void SceneSynchronizer::UpdateCameraCB(const RenderCameraProxy* camera)
    {
        m_cameraState.Update(camera);
    }

    void SceneSynchronizer::SubmitRenderProxies(std::vector<RenderProxy>&& proxies)
    {
        m_sceneSubmitter.SubmitRenderProxies(std::move(proxies));
    }

    void SceneSynchronizer::ClearSubmittedRenderProxies()
    {
        m_sceneSubmitter.ClearSubmittedRenderProxies();
        m_swrtExecutor.InvalidateCache();
    }

    void SceneSynchronizer::ClearRenderObjects()
    {
        ClearSubmittedRenderProxies();
    }

    SWRTExecutor::FrameContext SceneSynchronizer::BuildSwrtFrameContext() const
    {
        SWRTExecutor::FrameContext ctx{};
        std::memcpy(ctx.cameraPos,   m_cameraState.GetPos(),   sizeof(ctx.cameraPos));
        std::memcpy(ctx.cameraInvPV, m_cameraState.GetInvPV(), sizeof(ctx.cameraInvPV));
        ctx.viewportWidth  = m_viewport.Width;
        ctx.viewportHeight = m_viewport.Height;
        ctx.deltaTime      = m_deltaTime;
        return ctx;
    }
}
