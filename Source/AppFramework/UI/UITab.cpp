#include "UITab.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    Tab::Tab(const char* label)
    {
        m_selected = ImGui::BeginTabItem(label ? label : "Tab");
    }

    Tab::~Tab()
    {
        if (m_selected) {
            ImGui::EndTabItem();
        }
    }
}
