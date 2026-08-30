#include "UiTab.h"

#include "imgui.h"

namespace SasamiRenderer::UI
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
