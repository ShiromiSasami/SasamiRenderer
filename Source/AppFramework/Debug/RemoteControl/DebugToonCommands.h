#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the toon shading debug commands (toon, toon.apply) against `app`.
        // Handlers run on the main thread between frames.
        void RegisterToonCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
