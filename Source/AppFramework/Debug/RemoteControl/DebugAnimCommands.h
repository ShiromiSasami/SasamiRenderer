#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the CPU animation / gameplay debug commands (anim.selftest, gameplay.selftest,
        // anim.fox) against `app`. Handlers run on the main thread between frames.
        void RegisterAnimCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
