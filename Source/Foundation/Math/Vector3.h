#pragma once

namespace SasamiRenderer
{
    struct Vector3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        constexpr Vector3() = default;
        constexpr Vector3(float inX, float inY, float inZ)
            : x(inX), y(inY), z(inZ)
        {
        }

        constexpr Vector3& operator+=(const Vector3& rhs)
        {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            return *this;
        }
    };
}
