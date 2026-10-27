#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        class DebugCommandRegistry;

        // Registers the voxelization debug commands (voxel.selftest, voxel.scene, voxel.view) against `app`.
        // Handlers run on the main thread between frames; `app` must outlive the registry.
        void RegisterVoxelCommands(DebugCommandRegistry& registry, ApplicationCore& app);
    }
}
namespace SasamiEngine { using namespace App; }
