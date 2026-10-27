#include "UIGate.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    Gate::Gate(bool supported, const char* reason)
        : m_supported(supported)
        , m_reason(reason)
    {
        if (!m_supported) {
            ImGui::BeginDisabled(true);
        }
    }

    Gate::~Gate()
    {
        if (m_supported) {
            return;
        }

        ImGui::EndDisabled();

        // AllowWhenDisabled: without it the tooltip never appears, which is the whole point here.
        if (m_reason != nullptr && m_reason[0] != '\0' &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", m_reason);
        }
    }
}
