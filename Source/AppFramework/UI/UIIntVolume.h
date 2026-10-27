#pragma once

namespace SasamiEngine::App::UI
{
    // "Volume" part for integers: a bounded scalar control (an ImGui slider
    // under the hood). Returns true only when the edit is committed (slider
    // released, or a Ctrl+click direct input confirmed); stays false while
    // dragging so callers don't rebuild state every frame during a drag.
    bool IntVolume(const char* label, int* value, int min, int max, const char* format = "%d");
}
namespace SasamiEngine { using namespace App; }
