#pragma once

#include "UITabbedPanel.h"

#include <functional>
#include <string>
#include <vector>

namespace SasamiEngine::App::UI
{
    struct PanelInfo
    {
        std::string name;
        std::string category;
        size_t tabCount = 0;
        bool open = true;
    };

    // Every ImGui window an app registers, by name. A panel is one window made of tabs: adding a
    // tab to an existing name extends that window, and a second tab brings in a tab bar (TabbedPanel),
    // so an app adds a tab with one call instead of hand-writing BeginTabBar/EndTabBar.
    // Each panel has an open flag the editor's Window menu toggles. Begin gets no p_open, so a
    // window draws exactly as before (no close button); a closed panel is simply not begun.
    class UIPanelRegistry
    {
    public:
        // A window whose whole content is drawFn (a one-tab panel without tab-bar chrome).
        void AddWindow(const std::string& name, const std::string& category, std::function<void()> drawFn);
        // Appends a tab to window `windowName`, creating the window if needed. The tab bar id is
        // windowName + "Tabs". `category` only applies when the window is created here.
        void AddTab(const std::string& windowName, const std::string& category, const std::string& tabLabel,
                    std::function<void()> drawFn);

        // Begins every open panel in registration order and draws its tabs.
        void Draw();
        void Clear() { m_panels.clear(); }

        // False when no panel has that name.
        bool SetOpen(const std::string& name, bool open);
        bool IsOpen(const std::string& name) const;
        std::vector<PanelInfo> List() const;

    private:
        struct Panel
        {
            std::string name;
            std::string category;
            TabbedPanel tabs;
            size_t tabCount = 0;
            bool open = true;
        };

        Panel* Find(const std::string& name);
        const Panel* Find(const std::string& name) const;

        std::vector<Panel> m_panels;
    };

    // CPU checks of the registry bookkeeping (no ImGui calls). Returns failures, adds to `checks`.
    int RunPanelRegistrySelfTest(int& checks);
}
namespace SasamiEngine { using namespace App; }
