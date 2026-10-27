#include "AppFramework/Editor/EditorMainMenu.h"

#include "AppFramework/Editor/ContentBrowserPanel.h"
#include "AppFramework/Editor/ObjectInspectorPanel.h"
#include "AppFramework/Editor/SceneHierarchyPanel.h"
#include "AppFramework/Editor/EditorViewMode.h"
#include "AppFramework/UI/UIPanelRegistry.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace SasamiEngine::App
{
    namespace
    {
        void DrawWindowMenu(UI::UIPanelRegistry& panels, bool& showHierarchy, bool& showInspector,
                            bool& showContentBrowser, bool& resetLayout)
        {
            if (!ImGui::BeginMenu("Window"))
            {
                return;
            }

            ImGui::SeparatorText("Editor");
            ImGui::MenuItem(SceneHierarchyPanel::WINDOW_NAME, nullptr, &showHierarchy);
            ImGui::MenuItem(ObjectInspectorPanel::WINDOW_NAME, nullptr, &showInspector);
            ImGui::MenuItem(ContentBrowserPanel::WINDOW_NAME, nullptr, &showContentBrowser);
            if (ImGui::MenuItem("Reset Layout"))
            {
                resetLayout = true;
            }

            // Categories in order of first registration.
            const std::vector<UI::PanelInfo> infos = panels.List();
            std::vector<std::string> categories;
            for (const UI::PanelInfo& info : infos)
            {
                bool seen = false;
                for (const std::string& category : categories)
                {
                    seen = seen || category == info.category;
                }
                if (!seen)
                {
                    categories.push_back(info.category);
                }
            }

            for (const std::string& category : categories)
            {
                ImGui::SeparatorText(category.c_str());
                for (const UI::PanelInfo& info : infos)
                {
                    if (info.category != category)
                    {
                        continue;
                    }
                    bool open = info.open;
                    if (ImGui::MenuItem(info.name.c_str(), nullptr, &open))
                    {
                        panels.SetOpen(info.name, open);
                    }
                }
            }
            ImGui::EndMenu();
        }

        void DrawViewMenu(ApplicationCore& app)
        {
            if (!ImGui::BeginMenu("View"))
            {
                return;
            }
            const EditorViewMode current = CurrentEditorViewMode(app);
            const struct { EditorViewMode mode; const char* label; } items[] = {
                { EditorViewMode::Lit, "Lit" },
                { EditorViewMode::Unlit, "Unlit" },
                { EditorViewMode::Wireframe, "Wireframe" },
            };
            for (const auto& item : items)
            {
                if (ImGui::MenuItem(item.label, nullptr, current == item.mode))
                {
                    ApplyEditorViewMode(app, item.mode);
                }
            }
            ImGui::EndMenu();
        }
    }

    void EditorMainMenu::Draw(ApplicationCore& app, UI::UIPanelRegistry& panels, bool& showHierarchy, bool& showInspector,
                              bool& showContentBrowser, bool& resetLayout)
    {
        if (!ImGui::BeginMainMenuBar())
        {
            return;
        }
        DrawWindowMenu(panels, showHierarchy, showInspector, showContentBrowser, resetLayout);
        DrawViewMenu(app);
        ImGui::EndMainMenuBar();
    }
}
