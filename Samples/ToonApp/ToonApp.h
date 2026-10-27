#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Toon
{
    class ToonApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        // scene.load (debug pipe) clears every object and brings its own camera; m_camera would
        // dangle otherwise. Same check as RenderingApp::ReleaseReplacedSceneObjects.
        void RefreshCameraAfterSceneLoad(ApplicationCore& app);

        // Held so the ImGui window callback can reach the renderer; the callback outlives
        // OnInit's parameter but not the app itself.
        ApplicationCore* m_app = nullptr;
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
        Camera* m_camera = nullptr;
        uint64_t m_sceneGeneration = 0;

        float m_yaw   = 0.0f;
        float m_pitch = 0.15f;
        float m_camX  = 0.0f;
        float m_camY  = 1.5f;
        float m_camZ  = -7.5f;
    };
}
