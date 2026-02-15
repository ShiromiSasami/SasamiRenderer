#pragma once

#include <cstdint>
#include <cmath>
#include "Foundation/Math/Vector3.h"

namespace SasamiRenderer
{
    namespace Math
    {
        // Degree to radian conversion.
        // rad = deg * PI / 180
        inline float Deg(float degree) { return degree * 3.1415926535f / 180.0f; }

        // Row-major 4x4 multiplication (row-vector convention): out = a * b
        // out[r,c] = sum_{k=0..3} a[r,k] * b[k,c]
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

        inline void Cross(const float a[3], const float b[3], float out[3])
        {
            // 3D cross product:
            // out = a x b (right-handed)
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        inline void Normalize(float v[3])
        {
            // L2 normalization:
            // v := v / |v|, where |v| = sqrt(x^2 + y^2 + z^2)
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 0.0f) {
                v[0] /= len;
                v[1] /= len;
                v[2] /= len;
            }
        }

        inline Vector3 Cross(const Vector3& a, const Vector3& b)
        {
            // 3D cross product:
            // a x b gives vector orthogonal to both a and b.
            return Vector3(
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
        }

        inline void Normalize(Vector3& v)
        {
            // L2 normalization in-place:
            // v := v / sqrt(v.x^2 + v.y^2 + v.z^2)
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (len > 0.0f) {
                v.x /= len;
                v.y /= len;
                v.z /= len;
            }
        }

        inline Vector3 Normalize(const Vector3& v)
        {
            // Copy-and-normalize overload.
            Vector3 out = v;
            Normalize(out);
            return out;
        }

        inline void DirectionFromYawPitch(float yaw, float pitch, float outDirection[3])
        {
            // Yaw/Pitch to forward vector.
            // Current convention (engine-local):
            // x = -sin(yaw)
            // y =  sin(pitch) * cos(yaw)
            // z = -cos(pitch) * cos(yaw)
            // The result is normalized to protect against drift.
            const float cy = std::cos(yaw);
            const float sy = std::sin(yaw);
            const float cp = std::cos(pitch);
            const float sp = std::sin(pitch);

            outDirection[0] = -sy;
            outDirection[1] = sp * cy;
            outDirection[2] = -cp * cy;
            Normalize(outDirection);
        }

        inline void BuildDirectionalLightViewProjection(float yaw,
                                                        float pitch,
                                                        float distance,
                                                        float orthoHalf,
                                                        float nearZ,
                                                        float farZ,
                                                        float outLightVP[16],
                                                        float outForward[3])
        {
            // Build directional-light matrix used for shadow map projection.
            // 1) world: orientation from yaw/pitch
            // 2) view : translation by light distance
            // 3) proj : orthographic projection
            // Final: outLightVP = world * view * proj (row-major / row-vector)
            const float cy = std::cos(yaw);
            const float sy = std::sin(yaw);
            const float cp = std::cos(pitch);
            const float sp = std::sin(pitch);

            // Rotation basis from yaw/pitch.
            const float world[16] = {
                cy,      sp * sy,   -cp * sy,  0,
                0,       cp,         sp,       0,
                sy,     -sp * cy,    cp * cy,  0,
                0,       0,          0,        1,
            };

            // Light-space translation along local +Z by 'distance'.
            const float view[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,distance,1,
            };

            // Orthographic projection (row-major form).
            // x_ndc = 2*(x-l)/(r-l) - 1
            // y_ndc = 2*(y-b)/(t-b) - 1
            // z_ndc = (z-n)/(f-n)
            const float l = -orthoHalf;
            const float r = orthoHalf;
            const float b = -orthoHalf;
            const float t = orthoHalf;
            const float n = nearZ;
            const float f = farZ;
            const float proj[16] = {
                2/(r-l), 0,       0,        0,
                0,       2/(t-b), 0,        0,
                0,       0,      1/(f-n),   0,
                -(r+l)/(r-l), -(t+b)/(t-b), -n/(f-n), 1,
            };

            float wv[16];
            Mul4x4(world, view, wv);
            Mul4x4(wv, proj, outLightVP);

            // Also expose forward direction for shading/debug use.
            DirectionFromYawPitch(yaw, pitch, outForward);
        }

        inline uint16_t FloatToHalf(float f)
        {
            // IEEE754 float32 -> float16 conversion (round-toward-zero style via truncation).
            // Layout:
            // float32: sign(1) exponent(8) mantissa(23)
            // float16: sign(1) exponent(5) mantissa(10)
            union { float f; uint32_t u; } v = { f };
            const uint32_t sign = (v.u >> 31) & 0x1;
            const int32_t exp = static_cast<int32_t>((v.u >> 23) & 0xFF);
            uint32_t mantissa = v.u & 0x7FFFFF;

            const uint16_t outSign = static_cast<uint16_t>(sign << 15);
            if (exp == 255) {
                // Preserve Inf/NaN payload class.
                if (mantissa == 0) return static_cast<uint16_t>(outSign | 0x7C00);
                return static_cast<uint16_t>(outSign | 0x7C00 | (mantissa >> 13));
            }

            if (exp == 0) {
                // Treat float32 subnormal/zero as signed zero in current implementation.
                return outSign;
            }

            const int32_t newExp = exp - 127 + 15;
            if (newExp >= 31) {
                // Overflow to infinity in half.
                return static_cast<uint16_t>(outSign | 0x7C00);
            }
            if (newExp <= 0) {
                // Underflow path to half subnormal.
                if (newExp < -10) return outSign;
                mantissa |= 0x800000;
                const uint32_t shift = static_cast<uint32_t>(14 - newExp);
                const uint16_t halfMantissa = static_cast<uint16_t>(mantissa >> shift);
                return static_cast<uint16_t>(outSign | halfMantissa);
            }

            // Normalized half number.
            const uint16_t outExp = static_cast<uint16_t>(newExp << 10);
            const uint16_t outMantissa = static_cast<uint16_t>(mantissa >> 13);
            return static_cast<uint16_t>(outSign | outExp | outMantissa);
        }
    }
}
