#pragma once

namespace SasamiEngine::App::UI
{
    // "Vector3" part: an unbounded 3-component drag control (an ImGui drag
    // float3 under the hood). Returns true only when the edit is committed
    // (drag released, or a Ctrl+click direct input confirmed); stays false
    // while dragging so callers don't rebuild state every frame during a drag.
    bool Vector3(const char* label, float* xyz, float speed = 0.05f);
}
namespace SasamiEngine { using namespace App; }
