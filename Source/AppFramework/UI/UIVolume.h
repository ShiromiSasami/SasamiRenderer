#pragma once

namespace SasamiEngine::App::UI
{
    // "Volume" part: a bounded scalar control (an ImGui slider under the
    // hood). Covers both linear and logarithmic ranges so callers never
    // need to touch ImGuiSliderFlags directly. Returns true only when the
    // edit is committed (slider released, or a Ctrl+click direct input
    // confirmed); stays false while dragging so callers don't rebuild
    // state every frame during a drag.
    bool Volume(const char* label, float* value, float min, float max,
                const char* format = "%.3f", bool logarithmic = false);
}
namespace SasamiEngine { using namespace App; }
