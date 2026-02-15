#pragma once

#include <cmath>

namespace SasamiRenderer
{
    namespace Math
    {
        struct Float3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;

            constexpr Float3() = default;
            constexpr Float3(float inX, float inY, float inZ) : x(inX), y(inY), z(inZ) {}
        };

        inline Float3 operator+(const Float3& a, const Float3& b)
        {
            // Element-wise vector addition.
            return { a.x + b.x, a.y + b.y, a.z + b.z };
        }

        inline Float3 operator-(const Float3& a, const Float3& b)
        {
            // Element-wise vector subtraction.
            return { a.x - b.x, a.y - b.y, a.z - b.z };
        }

        inline Float3 operator*(const Float3& v, float s)
        {
            // Scalar multiplication.
            return { v.x * s, v.y * s, v.z * s };
        }

        inline Float3 operator*(float s, const Float3& v)
        {
            // Scalar multiplication (commutative overload).
            return { v.x * s, v.y * s, v.z * s };
        }

        inline Float3& operator+=(Float3& a, const Float3& b)
        {
            // In-place a := a + b.
            a.x += b.x;
            a.y += b.y;
            a.z += b.z;
            return a;
        }

        inline Float3& operator-=(Float3& a, const Float3& b)
        {
            // In-place a := a - b.
            a.x -= b.x;
            a.y -= b.y;
            a.z -= b.z;
            return a;
        }

        inline Float3& operator*=(Float3& v, float s)
        {
            // In-place scalar multiply.
            v.x *= s;
            v.y *= s;
            v.z *= s;
            return v;
        }

        inline float Dot(const Float3& a, const Float3& b)
        {
            // Dot product:
            // a.b = |a||b|cos(theta)
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        inline Float3 Cross(const Float3& a, const Float3& b)
        {
            // Cross product (right-handed):
            // |a x b| = |a||b|sin(theta)
            return {
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x,
            };
        }

        inline Float3 Normalize(const Float3& v)
        {
            // Safe normalization with epsilon guard.
            // Returns zero vector when input length is near zero.
            const float lenSq = Dot(v, v);
            if (lenSq <= 1e-20f) {
                return { 0.0f, 0.0f, 0.0f };
            }

            // invLen = 1 / sqrt(v.v), then v * invLen.
            const float invLen = 1.0f / std::sqrt(lenSq);
            return { v.x * invLen, v.y * invLen, v.z * invLen };
        }
    }
}
