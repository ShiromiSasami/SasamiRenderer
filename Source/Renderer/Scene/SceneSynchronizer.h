#pragma once
#include "Renderer/Scene/CameraState.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/RayTracing/RayTracingScene.h"
#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Renderer/Scene/RenderProxy.h"
#include <vector>

namespace SasamiRenderer
{
    class SceneSynchronizer
    {
    public:
        SceneSynchronizer(SceneSubmitter& sceneSubmitter,
                          CameraState& cameraState,
                          RayTracingScene& rayTracingScene,
                          SWRTExecutor& swrtExecutor,
                          DrawCommandBuilder& drawCommandBuilder,
                          const float& deltaTime,
                          const Viewport& viewport);

        void UpdateCameraCB(const RenderCameraProxy* camera);
        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();
        void ClearRenderObjects();
        SWRTExecutor::FrameContext BuildSwrtFrameContext() const;

    private:
        SceneSubmitter& m_sceneSubmitter;
        CameraState& m_cameraState;
        RayTracingScene& m_rayTracingScene;
        SWRTExecutor& m_swrtExecutor;
        DrawCommandBuilder& m_drawCommandBuilder;
        const float& m_deltaTime;
        const Viewport& m_viewport;
    };
}
