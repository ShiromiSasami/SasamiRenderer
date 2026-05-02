#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include <array>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiRenderer
{
    class StaticModel;

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
        StaticModel* m_sphereModel = nullptr;
        StaticModel* m_boxModel = nullptr;
        StaticModel* m_transparentSphereModel = nullptr;
        StaticModel* m_transparentBoxModel = nullptr;
        SurfaceMaterial m_sphereMaterial{};
        SurfaceMaterial m_boxMaterial{};
        SurfaceMaterial m_transparentSphereMaterial{};
        SurfaceMaterial m_transparentBoxMaterial{};
        bool m_showLightGizmo  = true;
        bool m_showLightGizmos = true; // Point / Spot light gizmos
        int m_probeGridPreset = 1;     // 0=Interior, 1=Wide, 2=Very Wide
        std::vector<boost::signals2::scoped_connection> m_inputConnections;
    };
}
