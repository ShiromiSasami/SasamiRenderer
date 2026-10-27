#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the CPU Position Based Fluids debug command (fluid.pbf).
        // Handlers run on the main thread between frames.
        void RegisterFluidCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
