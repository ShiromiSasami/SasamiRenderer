#pragma once

namespace SasamiEngine::App::UI
{
    // Color parts (ImGui color pickers under the hood). Returns true only
    // when the edit is committed (picker closed/released after a change);
    // stays false while dragging inside the popup so callers don't rebuild
    // state every frame during a drag.
    bool Color3(const char* label, float* rgb);
    bool Color4(const char* label, float* rgba);
}
namespace SasamiEngine { using namespace App; }
