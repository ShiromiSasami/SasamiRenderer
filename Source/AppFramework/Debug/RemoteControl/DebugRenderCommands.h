#pragma once

namespace SasamiRenderer
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the render-state debug commands (GBuffer debug view, exposure, IBL
        // intensity, AO mode, render path) against `app`.
        //
        // Split from the scene and capture commands because these poke renderer feature
        // settings rather than scene contents or frame capture.
        //
        // Every handler here runs on the main thread, because
        // DebugRemoteControlServer::DrainPendingCommands is what invokes them.
        //
        // `app` must outlive the registry.
        void RegisterRenderCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
