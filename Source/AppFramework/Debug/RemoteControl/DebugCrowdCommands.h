#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers crowd animation debug commands (crowd.bake) against `app`.
        // Handlers run on the main thread between frames; `app` must outlive the registry.
        void RegisterCrowdCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
