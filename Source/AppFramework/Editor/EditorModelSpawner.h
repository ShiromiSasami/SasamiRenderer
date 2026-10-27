#pragma once

#include "AppFramework/Component/MeshComponent.h"

#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;
    class StaticModel;

    // Adds a model from an Assets/-relative path to the scene, the same way FluidApp::LoadBistro does:
    // CreateStaticModel + synchronous LoadModel (FBX crashes on worker threads), DeleteObject on failure.
    // Loads block the frame; very large files (Dragon.obj, 112 MB) also risk the 5 s debug-command
    // timeout. Skinned glTF files load as static meshes in their bind pose.
    class EditorModelSpawner
    {
    public:
        // Places the model at `position`. Returns null and sets `error` on failure.
        static StaticModel* Spawn(ApplicationCore& app, const std::string& relPath, MeshComponent::ModelFormat format,
                                  const float position[3], std::string& error);

        // Point `distance` units in front of the main camera (last frame's proxy; origin without a camera).
        static void PointInFrontOfCamera(ApplicationCore& app, float distance, float outPosition[3]);
    };
}
