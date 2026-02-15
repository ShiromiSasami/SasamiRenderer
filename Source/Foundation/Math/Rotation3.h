#pragma once

namespace SasamiRenderer
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
