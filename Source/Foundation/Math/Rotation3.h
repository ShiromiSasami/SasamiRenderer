#pragma once

namespace SasamiEngine::Foundation
{
    struct Rotation3
    {
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;

        constexpr Rotation3() = default;
        constexpr Rotation3(float inPitch, float inYaw, float inRoll)
            : pitch(inPitch), yaw(inYaw), roll(inRoll)
        {
        }

    };
}

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
