#pragma once

namespace SasamiEngine::App::UI
{
    // "Angle" part: a bounded scalar control in radians displayed in degrees
    // (an ImGui slider angle under the hood). Returns true only when the
    // edit is committed (slider released, or a Ctrl+click direct input
    // confirmed); stays false while dragging so callers don't rebuild state
    // every frame during a drag.
    bool Angle(const char* label, float* radians, float degreesMin = -360.0f, float degreesMax = 360.0f,
               const char* format = "%.0f deg");
}
namespace SasamiEngine { using namespace App; }
