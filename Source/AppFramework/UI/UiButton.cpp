#include "UiButton.h"

#include "imgui.h"

namespace SasamiRenderer::UI
{
    bool Button(const char* label)
    {
        return ImGui::Button(label ? label : "Button");
    }
}
