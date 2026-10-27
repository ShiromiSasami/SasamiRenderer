#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the application-loop debug commands used by deterministic captures (G1):
        // app.fixeddt, app.ui, app.frame and app.ready.
        //
        // Split from RegisterCaptureCommands because these control the frame loop itself
        // (clock, frame counter, UI visibility), not what gets written to disk.
        //
        // `app` must outlive the registry.
        void RegisterAppCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
namespace SasamiEngine { using namespace App; }
