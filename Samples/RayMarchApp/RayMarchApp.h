#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Renderer/Passes/RayMarch/RayMarchRenderPass.h"
#include <memory>

namespace SasamiRenderer
{
    class RayMarchApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;
        void OnResize(ApplicationCore& app, UINT width, UINT height) override;

    private:
        Camera* m_camera = nullptr;
        std::shared_ptr<RayMarchRenderPass> m_renderPass;

        float m_yaw   = 0.0f;
        float m_pitch = -0.15f;
        float m_camX  = 0.0f;
        float m_camY  = 2.5f;
        float m_camZ  = -8.0f;

        bool m_mouseDown = false;
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;

        // Cone marching debug values (computed per-frame on CPU, same formula as shader)
        float m_dbgPixelConeAngle = 0.0f;
    };
}
