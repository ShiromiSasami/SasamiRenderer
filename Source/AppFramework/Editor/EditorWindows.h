#pragma once

#include "AppFramework/Editor/ContentBrowserPanel.h"
#include "AppFramework/Editor/EditorDockLayout.h"
#include "AppFramework/Editor/EditorGizmo.h"
#include "AppFramework/Editor/EditorMainMenu.h"
#include "AppFramework/Editor/EditorPlaySession.h"
#include "AppFramework/Editor/EditorPlayToolbar.h"
#include "AppFramework/Editor/EditorSelection.h"
#include "AppFramework/Editor/EditorSelectionOverlay.h"
#include "AppFramework/Editor/ObjectInspectorPanel.h"
#include "AppFramework/Editor/SceneHierarchyPanel.h"

namespace SasamiEngine::App
{
    class ApplicationCore;

    // Owns the editor selection and panels; ApplicationCore registers Draw as an ImGui overlay.
    // Hidden by default (Ctrl+E or `editor on` shows it), so existing captures stay unchanged.
    // While shown, a left click in the 3D view (not over ImGui, not a drag) picks an object
    // (EditorPicking), the selection is outlined (EditorSelectionOverlay) and a main menu bar
    // (EditorMainMenu) opens and closes the registered panels. The first time the dock space is empty,
    // EditorDockLayout docks WorldObjects / Details / Content Browser in UE5's default places; Window >
    // Reset Layout or `editor layout reset` rebuilds it. Closing the panels does not hide the editor;
    // Ctrl+E / `editor off` does. EditorPlayToolbar (top centre of the 3D view) starts and stops the
    // EditorPlaySession; while playing the panels are disabled, picking and the selection outline are
    // off, and Stop restores the scene.
    class EditorWindows
    {
    public:
        void Draw(ApplicationCore& app);

        // Showing the editor also reopens WorldObjects, Details and Content Browser.
        void SetVisible(bool visible);
        bool IsVisible() const { return m_visible; }

        EditorSelection& Selection() { return m_selection; }
        const EditorSelection& Selection() const { return m_selection; }

        // Picks at main-viewport pixel (x, y) and selects the hit, or clears on a miss. Returns the hit kind.
        EditorObjectKind PickAt(ApplicationCore& app, float x, float y);

        ContentBrowserPanel& ContentBrowser() { return m_contentBrowser; }
        void SetContentBrowserOpen(bool open) { m_showContentBrowser = open; }
        bool IsContentBrowserOpen() const { return m_showContentBrowser; }

        // Re-docks the three editor panels in the default layout (and reopens them) on the next Draw.
        void RequestLayoutReset();

        EditorPlaySession& PlaySession() { return m_playSession; }
        const EditorPlaySession& PlaySession() const { return m_playSession; }

        EditorGizmo& Gizmo() { return m_gizmo; }

    private:
        EditorSelection m_selection;
        SceneHierarchyPanel m_hierarchy;
        ObjectInspectorPanel m_inspector;
        ContentBrowserPanel m_contentBrowser;
        EditorSelectionOverlay m_selectionOverlay;
        EditorMainMenu m_mainMenu;
        EditorPlaySession m_playSession;
        EditorPlayToolbar m_playToolbar;
        EditorDockLayout m_dockLayout;
        EditorGizmo m_gizmo;
        bool m_visible = false;
        bool m_pressOnGizmo = false; // the current left press went to the gizmo, so its release does not pick
        bool m_showHierarchy = false;
        bool m_showInspector = false;
        bool m_showContentBrowser = false;
    };
}
