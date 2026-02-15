#pragma once

#include <wtypes.h>

namespace SasamiRenderer
{
    class ApplicationCore;

    class IApplication
    {
    public:
        virtual ~IApplication() = default;
        virtual void OnInit(ApplicationCore& app) { (void)app; }
        virtual void OnUpdate(ApplicationCore& app, float deltaTime) { (void)app; (void)deltaTime; }
        virtual void OnRender(ApplicationCore& app) { (void)app; }
        virtual void OnShutdown(ApplicationCore& app) { (void)app; }
        virtual void OnResize(ApplicationCore& app, UINT width, UINT height) { (void)app; (void)width; (void)height; }
    };
}
