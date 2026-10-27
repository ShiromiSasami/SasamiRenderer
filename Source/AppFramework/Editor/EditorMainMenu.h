#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;
    namespace UI { class UIPanelRegistry; }

    // The editor's main menu bar, drawn only while the editor is shown (so captures without the
    // editor are unchanged). Window lists the registered panels by category plus the editor's own
    // WorldObjects / Details / Content Browser and toggles their open flags (Reset Layout sets `resetLayout`);
    // View picks the EditorViewMode. Play / Stop sits in EditorPlayToolbar.
    class EditorMainMenu
    {
    public:
        void Draw(ApplicationCore& app, UI::UIPanelRegistry& panels, bool& showHierarchy, bool& showInspector,
                  bool& showContentBrowser, bool& resetLayout);
    };
}
