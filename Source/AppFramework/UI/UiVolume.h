#pragma once

namespace SasamiRenderer::UI
{
    // "Volume" part: a bounded scalar control (an ImGui slider under the
    // hood). Covers both linear and logarithmic ranges so callers never
    // need to touch ImGuiSliderFlags directly. Returns true on the frame
    // the value changes, matching ImGui::SliderFloat.
    bool Volume(const char* label, float* value, float min, float max,
                const char* format = "%.3f", bool logarithmic = false);
}
