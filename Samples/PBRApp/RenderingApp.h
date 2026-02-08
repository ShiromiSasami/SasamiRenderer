#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Renderer.h"
#include <memory>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiRenderer
{
    class RenderingApp : public IApplication
    {
    public:
        void OnInit(Application& app) override;
        void OnUpdate(Application& app, float deltaTime) override;
        void OnRender(Application& app) override;
        void OnShutdown(Application& app) override;
        void OnResize(Application& app, UINT width, UINT height) override;

    private:
        void BindInputEvents();
        void RegisterUi();

        std::unique_ptr<Renderer> m_renderer;
        Camera m_camera;
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
    };
}
