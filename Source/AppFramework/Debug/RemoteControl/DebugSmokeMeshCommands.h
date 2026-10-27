#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers smoke.mesh: reads the GPU smoke density back and meshes one iso level of it
        // (marching tetrahedra), optionally adding the mesh to the scene. Main thread, between frames.
        void RegisterSmokeMeshCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}

namespace SasamiEngine { using namespace App; }
