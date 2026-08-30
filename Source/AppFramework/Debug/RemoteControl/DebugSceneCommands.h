#pragma once

namespace SasamiRenderer
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the app-layer debug commands (camera, scene) against `app`.
        //
        // Every handler registered here runs on the main thread, because
        // DebugRemoteControlServer::DrainPendingCommands is what invokes them. Touching
        // the scene or camera from the transport thread would be a data race.
        //
        // `app` must outlive the registry.
        void RegisterSceneCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
