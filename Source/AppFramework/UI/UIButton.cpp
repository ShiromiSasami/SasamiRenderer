#include "UIButton.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    bool Button(const char* label)
    {
        return ImGui::Button(label ? label : "Button");
    }
}
