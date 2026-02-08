#pragma once

#include <wtypes.h>

namespace SasamiRenderer
{
    class Application;

    class IApplication
    {
    public:
        virtual ~IApplication() = default;
        virtual void OnInit(Application& app) { (void)app; }
        virtual void OnUpdate(Application& app, float deltaTime) { (void)app; (void)deltaTime; }
        virtual void OnRender(Application& app) { (void)app; }
        virtual void OnShutdown(Application& app) { (void)app; }
        virtual void OnResize(Application& app, UINT width, UINT height) { (void)app; (void)width; (void)height; }
    };
}
