#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include <memory>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::NeuralShader
{
    class NeuralShaderApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
        Camera* m_camera = nullptr;

        float m_yaw   = 0.0f;
        float m_pitch = 0.15f;
        float m_camX  = 0.0f;
        float m_camY  = 1.5f;
        float m_camZ  = -7.5f;
    };
}
