#include "UiVolume.h"

#include "imgui.h"

namespace SasamiRenderer::UI
{
    bool Volume(const char* label, float* value, float min, float max,
                const char* format, bool logarithmic)
    {
        const ImGuiSliderFlags flags = logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
        return ImGui::SliderFloat(label, value, min, max, format, flags);
    }
}
