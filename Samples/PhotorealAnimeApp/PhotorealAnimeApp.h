#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include <memory>

namespace SasamiRenderer
{
    class PhotorealAnimeApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;
        void OnResize(ApplicationCore& app, UINT width, UINT height) override;

    private:
        Camera* m_camera = nullptr;

        float m_yaw   = 0.0f;
        float m_pitch = 0.15f;
        float m_camX  = 0.0f;
        float m_camY  = 1.5f;
        float m_camZ  = -7.5f;

        bool m_mouseDown = false;
        float m_lastMouseX = 0.0f;
        float m_lastMouseY = 0.0f;
    };
}
