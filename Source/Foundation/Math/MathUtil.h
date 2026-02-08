#pragma once

namespace SasamiRenderer
{
    namespace Math
    {
        // Degrees to radians.
        inline float Deg(float degree) { return degree * 3.1415926535f / 180.0f; }

        // Row-major 4x4 multiply: out = a * b
        inline void Mul4x4(const float a[16], const float b[16], float out[16])
        {
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c]
                                   + a[r * 4 + 1] * b[1 * 4 + c]
                                   + a[r * 4 + 2] * b[2 * 4 + c]
                                   + a[r * 4 + 3] * b[3 * 4 + c];
                }
            }
        }
    }
}
