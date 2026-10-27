#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Object/StaticModel.h"
#include <memory>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Fluid
{
    class FluidApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        void DrawWaterTab(ApplicationCore& app);
        void DrawSmokeTab(ApplicationCore& app);
        void DrawBistroControls(ApplicationCore& app);
        void RegisterBistroCommand(ApplicationCore& app);
        bool LoadBistro(ApplicationCore& app, std::string& error);

        std::vector<boost::signals2::scoped_connection> m_inputConnections;
        Camera* m_camera = nullptr;
        StaticModel* m_bistroModel = nullptr;
        std::string m_bistroError;
        float m_smokeWindDir[3] = {1.0f, 0.0f, 0.0f};

        float m_yaw   = 0.0f;
        float m_pitch = 0.2f;
        float m_camX  = 0.0f;
        float m_camY  = 2.5f;
        float m_camZ  = -10.0f;
    };
}
