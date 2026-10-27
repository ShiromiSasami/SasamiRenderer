#pragma once

#include "AppFramework/Editor/ContentAssetCatalog.h"
#include "AppFramework/Editor/EditorSelection.h"

#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    // "Content Browser" window: the Assets/ model files as a folder tree (ContentAssetCatalog, scanned
    // on first open and on Refresh). Double-clicking a file adds it in front of the camera
    // (EditorModelSpawner) and selects it. Loads are synchronous, so the frame stalls meanwhile.
    class ContentBrowserPanel
    {
    public:
        static constexpr const char* WINDOW_NAME = "Content Browser";

        void Draw(ApplicationCore& app, EditorSelection& selection, bool* open);

        ContentAssetCatalog& Catalog() { return m_catalog; }

        // Shared by the double-click and `editor content add`. Returns "OK ..." or "ERR ...".
        std::string AddToScene(ApplicationCore& app, EditorSelection& selection, const std::string& relPath);

    private:
        void DrawFolder(ApplicationCore& app, EditorSelection& selection, const ContentFolder& folder);

        ContentAssetCatalog m_catalog;
        std::string m_status;
    };
}
