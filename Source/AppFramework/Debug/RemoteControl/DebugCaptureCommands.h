#pragma once

namespace SasamiRenderer
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the frame-capture debug commands (screenshot) against `app`.
        //
        // Split from RegisterSceneCommands because capture is a renderer-side concern with
        // its own two-step request/poll protocol, not scene state manipulation.
        //
        // Like the scene commands, every handler here runs on the main thread because
        // DebugRemoteControlServer::DrainPendingCommands is what invokes them.
        //
        // `app` must outlive the registry.
        void RegisterCaptureCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
