#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include <array>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiRenderer
{
    class RenderingApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;
        void OnResize(ApplicationCore& app, UINT width, UINT height) override;

    private:
        enum class SkyboxLoadFormat
        {
            Auto = 0,
            HdrEquirect = 1,
            LdrEquirect = 2,
            CubemapFaces = 3,
        };

        void ApplySkyboxSettings(ApplicationCore& app);
        void BindInputEvents(ApplicationCore& app);
        void RegisterUi(ApplicationCore& app);

        Camera* m_camera = nullptr;
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
    };
}
