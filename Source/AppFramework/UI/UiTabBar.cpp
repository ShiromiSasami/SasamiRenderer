#include "UiTabBar.h"

#include "imgui.h"

namespace SasamiRenderer::UI
{
    TabBar::TabBar(const char* id)
    {
        m_open = ImGui::BeginTabBar(id ? id : "TabBar");
    }

    TabBar::~TabBar()
    {
        if (m_open) {
            ImGui::EndTabBar();
        }
    }
}
