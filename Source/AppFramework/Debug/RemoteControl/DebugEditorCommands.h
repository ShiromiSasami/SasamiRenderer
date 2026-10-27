#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the editor window commands (editor) against `app`.
        // Handlers run on the main thread between frames.
        void RegisterEditorCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
