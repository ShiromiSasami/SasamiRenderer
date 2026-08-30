#pragma once
#include <functional>
#include <string>
#include <vector>

namespace SasamiRenderer::UI
{
    // Declarative collection of named "tabs" — the parts-level building
    // block for the "tab (window)" concept: stuff parts into a tab, and
    // if more than one tab is registered a tab bar with tab buttons
    // appears automatically, without the caller ever calling ImGui's
    // Begin/EndTabBar or Begin/EndTabItem.
    //
    //   UI::TabbedPanel panel("PBRControlTabs");
    //   panel.AddTab("Camera", [&]{ ... });
    //   panel.AddTab("Lighting", [&]{ ... });
    //   panel.Draw(); // call once per frame
    //
    // With exactly one registered tab, Draw() renders its content
    // directly (no tab-bar chrome). With two or more, Draw() wraps every
    // registered tab in a TabBar/Tab pair automatically.
    class TabbedPanel
    {
    public:
        explicit TabbedPanel(std::string tabBarId);

        void AddTab(std::string label, std::function<void()> drawFn);
        void Draw() const;

    private:
        std::string m_tabBarId;
        std::vector<std::pair<std::string, std::function<void()>>> m_tabs;
    };
}
