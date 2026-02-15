#pragma once

namespace SasamiRenderer
{
    struct Color
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;

        constexpr Color() = default;
        constexpr Color(float inR, float inG, float inB, float inA = 1.0f)
            : r(inR), g(inG), b(inB), a(inA)
        {
        }

        constexpr float* Data() { return &r; }
        constexpr const float* Data() const { return &r; }
    };
}
