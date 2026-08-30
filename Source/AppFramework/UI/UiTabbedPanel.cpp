#include "UiTabbedPanel.h"

#include "UiTab.h"
#include "UiTabBar.h"

namespace SasamiRenderer::UI
{
    TabbedPanel::TabbedPanel(std::string tabBarId)
        : m_tabBarId(std::move(tabBarId))
    {
    }

    void TabbedPanel::AddTab(std::string label, std::function<void()> drawFn)
    {
        m_tabs.emplace_back(std::move(label), std::move(drawFn));
    }

    void TabbedPanel::Draw() const
    {
        if (m_tabs.empty()) {
            return;
        }

        if (m_tabs.size() == 1) {
            // A single tab has nothing to switch between, so it is drawn as
            // plain content with no tab-bar chrome.
            if (m_tabs.front().second) {
                m_tabs.front().second();
            }
            return;
        }

        TabBar bar(m_tabBarId.c_str());
        if (!bar) {
            return;
        }
        for (const auto& [label, drawFn] : m_tabs) {
            Tab tab(label.c_str());
            if (tab && drawFn) {
                drawFn();
            }
        }
    }
}
