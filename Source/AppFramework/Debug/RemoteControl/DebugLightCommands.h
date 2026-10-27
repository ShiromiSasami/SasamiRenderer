#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the directional-light debug commands (direction, intensity, ortho half-size,
        // depth range, shadow bias, cascade tuning) against `app`.
        //
        // Split from DebugRenderCommands because render.* pokes renderer feature settings (GBuffer
        // debug view, exposure, IBL, AO mode, render path) whereas light.* pokes the scene's
        // directional light state itself.
        //
        // Every handler here runs on the main thread, because
        // DebugRemoteControlServer::DrainPendingCommands is what invokes them.
        //
        // `app` must outlive the registry.
        void RegisterLightCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
namespace SasamiEngine { using namespace App; }
