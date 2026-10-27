#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace SasamiEngine::App
{
    // Builds the editor's default dock layout on the main dock space, after UE5's Level Editor:
    // WorldObjects top right, Details below it, Content Browser along the bottom, the 3D view in the
    // (passthrough) central node. Apply() builds it only while the dock space is still empty, so a
    // layout the user arranged (and imgui.ini saved) is kept; RequestReset() rebuilds it once.
    // The app's own panels (UIPanelRegistry, e.g. FluidApp's "Fluid") are docked as extra tabs in the
    // Details node, as UE tabs World Settings next to Details; Details stays the selected tab.
    class EditorDockLayout
    {
    public:
        // Call before the editor windows Begin, in a frame whose dock space is already submitted.
        // Returns true when it built the layout this frame.
        // appPanels: window names docked as tabs beside Details when the layout is built.
        bool Apply(ImGuiID dockSpaceId, const std::vector<std::string>& appPanels);
        void RequestReset() { m_resetRequested = true; }

    private:
        static void Build(ImGuiID dockSpaceId, const std::vector<std::string>& appPanels);

        bool m_resetRequested = false;
    };
}
