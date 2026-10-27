#include "UITabBar.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
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
