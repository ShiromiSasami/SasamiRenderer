#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the particle-character debug commands (pchar.selftest, pchar.bake, pchar.sim).
        // Handlers run on the main thread between frames; `app` must outlive the registry.
        void RegisterParticleCharacterCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
