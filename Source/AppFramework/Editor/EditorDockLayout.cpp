#include "AppFramework/Editor/EditorDockLayout.h"

#include "AppFramework/Editor/ContentBrowserPanel.h"
#include "AppFramework/Editor/ObjectInspectorPanel.h"
#include "AppFramework/Editor/SceneHierarchyPanel.h"

#include "imgui_internal.h"

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float RIGHT_COLUMN_RATIO = 0.22f;    // of the viewport width
        constexpr float DETAILS_RATIO = 0.55f;         // of the right column's height
        constexpr float CONTENT_BROWSER_RATIO = 0.28f; // of the remaining (left) height
    }

    bool EditorDockLayout::Apply(ImGuiID dockSpaceId, const std::vector<std::string>& appPanels)
    {
        if (dockSpaceId == 0)
        {
            return false;
        }
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockSpaceId);
        const bool empty = node == nullptr || (node->IsLeafNode() && node->Windows.Size == 0);
        if (!m_resetRequested && !empty)
        {
            return false;
        }
        m_resetRequested = false;
        Build(dockSpaceId, appPanels);
        return true;
    }

    void EditorDockLayout::Build(ImGuiID dockSpaceId, const std::vector<std::string>& appPanels)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::DockBuilderRemoveNode(dockSpaceId);
        ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodeSize(dockSpaceId, viewport->WorkSize);

        ImGuiID left = 0;
        const ImGuiID right = ImGui::DockBuilderSplitNode(dockSpaceId, ImGuiDir_Right, RIGHT_COLUMN_RATIO, nullptr, &left);
        ImGuiID outliner = 0;
        const ImGuiID details = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, DETAILS_RATIO, nullptr, &outliner);
        const ImGuiID content = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, CONTENT_BROWSER_RATIO, nullptr, nullptr);

        ImGui::DockBuilderDockWindow(SceneHierarchyPanel::WINDOW_NAME, outliner);
        ImGui::DockBuilderDockWindow(ObjectInspectorPanel::WINDOW_NAME, details);
        ImGui::DockBuilderDockWindow(ContentBrowserPanel::WINDOW_NAME, content);
        for (const std::string& name : appPanels)
        {
            ImGui::DockBuilderDockWindow(name.c_str(), details);
        }
        // A recreated tab bar selects node->SelectedTabId when set, else the last submitted tab, which
        // would be whichever panel Begins last. ImGuiWindow::TabId is GetID("#TAB") inside the window,
        // i.e. ImHashStr("#TAB", 0, windowId) with windowId = ImHashStr(name).
        if (ImGuiDockNode* detailsNode = ImGui::DockBuilderGetNode(details))
        {
            detailsNode->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr(ObjectInspectorPanel::WINDOW_NAME));
        }
        ImGui::DockBuilderFinish(dockSpaceId);
    }
}
