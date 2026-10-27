#pragma once

#include "imgui.h"

namespace SasamiEngine::App
{
    class ApplicationCore;
    class EditorGizmo;
    class EditorPlaySession;

    // UE-style Play / Stop toolbar: a small undecorated window at the top centre of the dock space's
    // central node (the 3D view), or of the viewport's work area when there is no dock space.
    // Draw it outside the editor's BeginDisabled block, since it has to stay clickable while playing.
    // While not playing it also carries the gizmo's T / R / S and Local / World buttons.
    class EditorPlayToolbar
    {
    public:
        void Draw(ApplicationCore& app, EditorPlaySession& playSession, EditorGizmo& gizmo, ImGuiID dockSpaceId);
    };
}
