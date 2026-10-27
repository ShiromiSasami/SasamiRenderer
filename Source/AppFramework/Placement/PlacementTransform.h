#pragma once

#include <array>
#include <cmath>

namespace SasamiEngine::App
{
    using PlacementMatrix = std::array<float, 16>;

    // Yaw about +Y, uniform scale, then translation: the matrix a 5-value [instance] line describes
    // (ApplicationCoreScene.cpp ParseInstanceTransform). Row-major, row-vector convention, so row 0
    // is where local +X goes, row 2 where local +Z goes -- (sin yaw, 0, cos yaw) -- and row 3 is
    // the translation.
    inline PlacementMatrix MakeYawScaleTranslation(float yaw, float scale, float x, float y, float z)
    {
        const float c = std::cos(yaw) * scale;
        const float s = std::sin(yaw) * scale;
        return {
            c,    0.0f,  -s,   0.0f,
            0.0f, scale, 0.0f, 0.0f,
            s,    0.0f,  c,    0.0f,
            x,    y,     z,    1.0f,
        };
    }
}
