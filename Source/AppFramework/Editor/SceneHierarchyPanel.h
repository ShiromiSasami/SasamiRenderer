#pragma once

#include "AppFramework/Editor/EditorSelection.h"

namespace SasamiEngine::App
{
    class ApplicationCore;

    // "WorldObjects" window (UE calls it Outliner, Unity Hierarchy): lists the directional light, point / spot lights, static / skinned models and
    // cameras of the ApplicationCore, and writes the clicked entry into the selection.
    class SceneHierarchyPanel
    {
    public:
        static constexpr const char* WINDOW_NAME = "WorldObjects";

        // Drops the selection when the scene was reloaded or the object is no longer listed.
        static void ValidateSelection(ApplicationCore& app, EditorSelection& selection);

        // Draws the window (Begin/End inside). `open` is the window's close button state.
        void Draw(ApplicationCore& app, EditorSelection& selection, bool* open);
    };
}
