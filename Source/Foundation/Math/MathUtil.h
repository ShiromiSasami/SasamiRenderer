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

        inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

        // Default reflection strength from roughness/metallic: metallic surfaces reflect,
        // and higher roughness scatters/weakens the reflection.
        inline float DefaultReflectionStrength(float roughness, float metallic)
        {
            return Clamp01(Clamp01(metallic) * (1.0f - Clamp01(roughness)));
        }

        inline float Dot3(const float a[3], const float b[3])
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        inline void Lerp3(const float a[3], const float b[3], float t, float out[3])
        {
            out[0] = a[0] + (b[0] - a[0]) * t;
            out[1] = a[1] + (b[1] - a[1]) * t;
            out[2] = a[2] + (b[2] - a[2]) * t;
        }

        inline float RoundToMultiple(float value, float step)
        {
            if (step <= 1e-7f) {
                return value;
            }
            return std::floor(value / step + 0.5f) * step;
        }

        // Transforms a point by a row-major 4x4 matrix (row-vector convention) with
        // perspective divide; falls back to the un-divided result if w is degenerate.
        inline void TransformPoint(const float m[16], const float p[3], float out[3])
        {
            const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8]  + m[12];
            const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9]  + m[13];
            const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
            const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];

            if (std::fabs(w) > 1e-7f) {
                out[0] = x / w;
                out[1] = y / w;
                out[2] = z / w;
            } else {
                out[0] = x;
                out[1] = y;
                out[2] = z;
            }
        }

        // Row-major 4x4 identity matrix.
        inline void Identity4x4(float out[16])
        {
            for (int i = 0; i < 16; ++i) {
                out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
        }

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

        inline bool Invert4x4(const float m[16], float out[16])
        {
            const float inv[16] = {
                m[5]  * m[10] * m[15] -
                m[5]  * m[11] * m[14] -
                m[9]  * m[6]  * m[15] +
                m[9]  * m[7]  * m[14] +
                m[13] * m[6]  * m[11] -
                m[13] * m[7]  * m[10],

               -m[1]  * m[10] * m[15] +
                m[1]  * m[11] * m[14] +
                m[9]  * m[2] * m[15] -
                m[9]  * m[3] * m[14] -
                m[13] * m[2] * m[11] +
                m[13] * m[3] * m[10],

                m[1]  * m[6] * m[15] -
                m[1]  * m[7] * m[14] -
                m[5]  * m[2] * m[15] +
                m[5]  * m[3] * m[14] +
                m[13] * m[2] * m[7] -
                m[13] * m[3] * m[6],

               -m[1]  * m[6] * m[11] +
                m[1]  * m[7] * m[10] +
                m[5]  * m[2] * m[11] -
                m[5]  * m[3] * m[10] -
                m[9]  * m[2] * m[7] +
                m[9]  * m[3] * m[6],

               -m[4]  * m[10] * m[15] +
                m[4]  * m[11] * m[14] +
                m[8]  * m[6] * m[15] -
                m[8]  * m[7] * m[14] -
                m[12] * m[6] * m[11] +
                m[12] * m[7] * m[10],

                m[0]  * m[10] * m[15] -
                m[0]  * m[11] * m[14] -
                m[8]  * m[2] * m[15] +
                m[8]  * m[3] * m[14] +
                m[12] * m[2] * m[11] -
                m[12] * m[3] * m[10],

               -m[0]  * m[6] * m[15] +
                m[0]  * m[7] * m[14] +
                m[4]  * m[2] * m[15] -
                m[4]  * m[3] * m[14] -
                m[12] * m[2] * m[7] +
                m[12] * m[3] * m[6],

                m[0]  * m[6] * m[11] -
                m[0]  * m[7] * m[10] -
                m[4]  * m[2] * m[11] +
                m[4]  * m[3] * m[10] +
                m[8]  * m[2] * m[7] -
                m[8]  * m[3] * m[6],

                m[4]  * m[9] * m[15] -
                m[4]  * m[11] * m[13] -
                m[8]  * m[5] * m[15] +
                m[8]  * m[7] * m[13] +
                m[12] * m[5] * m[11] -
                m[12] * m[7] * m[9],

               -m[0]  * m[9] * m[15] +
                m[0]  * m[11] * m[13] +
                m[8]  * m[1] * m[15] -
                m[8]  * m[3] * m[13] -
                m[12] * m[1] * m[11] +
                m[12] * m[3] * m[9],

                m[0]  * m[5] * m[15] -
                m[0]  * m[7] * m[13] -
                m[4]  * m[1] * m[15] +
                m[4]  * m[3] * m[13] +
                m[12] * m[1] * m[7] -
                m[12] * m[3] * m[5],

               -m[0]  * m[5] * m[11] +
                m[0]  * m[7] * m[9] +
                m[4]  * m[1] * m[11] -
                m[4]  * m[3] * m[9] -
                m[8]  * m[1] * m[7] +
                m[8]  * m[3] * m[5],

               -m[4]  * m[9] * m[14] +
                m[4]  * m[10] * m[13] +
                m[8]  * m[5] * m[14] -
                m[8]  * m[6] * m[13] -
                m[12] * m[5] * m[10] +
                m[12] * m[6] * m[9],

                m[0]  * m[9] * m[14] -
                m[0]  * m[10] * m[13] -
                m[8]  * m[1] * m[14] +
                m[8]  * m[2] * m[13] +
                m[12] * m[1] * m[10] -
                m[12] * m[2] * m[9],

               -m[0]  * m[5] * m[14] +
                m[0]  * m[6] * m[13] +
                m[4]  * m[1] * m[14] -
                m[4]  * m[2] * m[13] -
                m[12] * m[1] * m[6] +
                m[12] * m[2] * m[5],

                m[0]  * m[5] * m[10] -
                m[0]  * m[6] * m[9] -
                m[4]  * m[1] * m[10] +
                m[4]  * m[2] * m[9] +
                m[8]  * m[1] * m[6] -
                m[8]  * m[2] * m[5]
            };

            const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
            if (std::fabs(det) <= 1e-8f) {
                return false;
            }

            const float invDet = 1.0f / det;
            for (int i = 0; i < 16; ++i) {
                out[i] = inv[i] * invDet;
            }
            return true;
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
            // Standard spherical convention, matching Camera::ComputeForward
            // (Source/AppFramework/Object/Camera.cpp), which is the confirmed-correct
            // reference (verified against the real camera view on hardware):
            // x =  sin(yaw) * cos(pitch)
            // y = -sin(pitch)
            // z =  cos(yaw) * cos(pitch)
            // (Sign convention is engine-specific: +pitch looks downward.)
            // The result is normalized to protect against drift.
            const float cy = std::cos(yaw);
            const float sy = std::sin(yaw);
            const float cp = std::cos(pitch);
            const float sp = std::sin(pitch);

            outDirection[0] = sy * cp;
            outDirection[1] = -sp;
            outDirection[2] = cy * cp;
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
            DirectionFromYawPitch(yaw, pitch, outForward);

            float referenceUp[3] = { 0.0f, 1.0f, 0.0f };
            if (std::fabs(outForward[1]) > 0.99f) {
                referenceUp[0] = 0.0f;
                referenceUp[1] = 0.0f;
                referenceUp[2] = 1.0f;
            }

            float right[3] = {};
            Cross(outForward, referenceUp, right);
            Normalize(right);

            float up[3] = {};
            Cross(right, outForward, up);
            Normalize(up);

            // World -> light-view rotation for row-vector convention.
            // Each column stores one light-space axis in world space so that:
            // x' = dot(worldPos, right), y' = dot(worldPos, up), z' = dot(worldPos, forward)
            const float world[16] = {
                right[0],      up[0],      outForward[0], 0,
                right[1],      up[1],      outForward[1], 0,
                right[2],      up[2],      outForward[2], 0,
                0,             0,          0,             1,
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
        }

        // Row-major perspective VP matrix for a spot light shadow camera.
        // Convention: same as BuildDirectionalLightViewProjection (row-vector, LH, 0-1 depth).
        // @param yaw, pitch   spot light orientation (same convention as DirectionFromYawPitch)
        // @param pos          spot light world-space position (float[3])
        // @param halfAngle    outer cone half-angle in radians (determines shadow FOV)
        // @param nearZ, farZ  depth range for the perspective frustum
        // @param outVP        output 16-float row-major view-projection matrix
        inline void BuildSpotLightViewProjection(float yaw,
                                                 float pitch,
                                                 const float pos[3],
                                                 float halfAngle,
                                                 float nearZ,
                                                 float farZ,
                                                 float outVP[16])
        {
            float fwd[3] = {};
            DirectionFromYawPitch(yaw, pitch, fwd);

            float referenceUp[3] = { 0.0f, 1.0f, 0.0f };
            if (std::fabs(fwd[1]) > 0.99f) {
                referenceUp[0] = 0.0f;
                referenceUp[1] = 0.0f;
                referenceUp[2] = 1.0f;
            }

            // NOTE: This computes right = fwd x up, which is the mirror image of the
            // right = up x fwd used by DirectXMath's XMMatrixLookToLH (LH convention).
            // It is intentionally left as-is: the shadow map render pass and the shader-side
            // world-to-light-space projection both consume this same VP matrix, so the mirrored
            // basis is self-consistent, and the shadow PSO sets D3D12_CULL_MODE_NONE
            // (see RenderPipelineStateCache.cpp), so the resulting winding-order flip is never
            // culled. Do NOT "fix" this ordering. If back-face culling is ever enabled for
            // shadow rendering, this basis must be swapped to the LH convention first.
            float right[3] = {};
            Cross(fwd, referenceUp, right);
            Normalize(right);

            float up[3] = {};
            Cross(right, fwd, up);
            Normalize(up);

            // Combined view matrix (rotation + position, row-major row-vector convention).
            // worldPos * view = lightSpacePos
            const float tx = -(right[0]*pos[0] + right[1]*pos[1] + right[2]*pos[2]);
            const float ty = -(up[0]   *pos[0] + up[1]   *pos[1] + up[2]   *pos[2]);
            const float tz = -(fwd[0]  *pos[0] + fwd[1]  *pos[1] + fwd[2]  *pos[2]);

            const float view[16] = {
                right[0], up[0], fwd[0], 0.0f,
                right[1], up[1], fwd[1], 0.0f,
                right[2], up[2], fwd[2], 0.0f,
                tx,       ty,    tz,     1.0f,
            };

            // Perspective projection (LH, 0-1 depth, square frustum, row-major).
            const float invTan = (halfAngle > 0.0f) ? (1.0f / std::tan(halfAngle)) : 1.0f;
            const float n = nearZ, f = farZ;
            const float proj[16] = {
                invTan, 0.0f,   0.0f,          0.0f,
                0.0f,   invTan, 0.0f,          0.0f,
                0.0f,   0.0f,   f / (f - n),   1.0f,
                0.0f,   0.0f,  -n*f / (f - n), 0.0f,
            };

            Mul4x4(view, proj, outVP);
        }

        // Row-major perspective VP matrix for one face of a point light cube shadow map.
        // Convention: same row-vector/LH/0-1-depth convention as BuildSpotLightViewProjection.
        // Face index order matches the D3D cubemap array-slice convention: 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z.
        // Each face uses a fixed axis-aligned forward/up pair, so (unlike the spot-light case) no
        // singularity-avoidance branch is needed.
        // @param pos          point light world-space position (float[3])
        // @param faceIndex    cube face index (0-5), see convention above
        // @param nearZ, farZ  depth range for the perspective frustum
        // @param outVP        output 16-float row-major view-projection matrix
        inline void BuildCubeFaceViewProjection(const float pos[3],
                                                 uint32_t faceIndex,
                                                 float nearZ,
                                                 float farZ,
                                                 float outVP[16])
        {
            static constexpr float kFaceForward[6][3] = {
                {  1.0f,  0.0f,  0.0f }, // +X
                { -1.0f,  0.0f,  0.0f }, // -X
                {  0.0f,  1.0f,  0.0f }, // +Y
                {  0.0f, -1.0f,  0.0f }, // -Y
                {  0.0f,  0.0f,  1.0f }, // +Z
                {  0.0f,  0.0f, -1.0f }, // -Z
            };
            static constexpr float kFaceUp[6][3] = {
                { 0.0f, 1.0f,  0.0f }, // +X
                { 0.0f, 1.0f,  0.0f }, // -X
                { 0.0f, 0.0f, -1.0f }, // +Y
                { 0.0f, 0.0f,  1.0f }, // -Y
                { 0.0f, 1.0f,  0.0f }, // +Z
                { 0.0f, 1.0f,  0.0f }, // -Z
            };

            const uint32_t face = faceIndex < 6u ? faceIndex : 0u;
            const float* fwd = kFaceForward[face];
            const float* up = kFaceUp[face];

            // NOTE: right = fwd x up here, using the fixed kFaceUp table above, which is the
            // mirror of the right = up x fwd used by DirectXMath's XMMatrixLookToLH (LH
            // convention). This is intentional, not a bug: the cube shadow map is rendered and
            // sampled with this exact VP matrix, so the mirrored basis is self-consistent, and
            // the shadow PSO uses D3D12_CULL_MODE_NONE (see RenderPipelineStateCache.cpp), so the
            // per-face winding-order flip is never culled. Do NOT reorder this Cross() call. If
            // back-face culling is ever enabled for point-light shadow rendering, this basis must
            // be converted to the LH convention first.
            float right[3] = {};
            Cross(fwd, up, right);
            Normalize(right);

            // Combined view matrix (rotation + position, row-major row-vector convention).
            // worldPos * view = lightSpacePos
            const float tx = -(right[0]*pos[0] + right[1]*pos[1] + right[2]*pos[2]);
            const float ty = -(up[0]   *pos[0] + up[1]   *pos[1] + up[2]   *pos[2]);
            const float tz = -(fwd[0]  *pos[0] + fwd[1]  *pos[1] + fwd[2]  *pos[2]);

            const float view[16] = {
                right[0], up[0], fwd[0], 0.0f,
                right[1], up[1], fwd[1], 0.0f,
                right[2], up[2], fwd[2], 0.0f,
                tx,       ty,    tz,     1.0f,
            };

            // Perspective projection (LH, 0-1 depth, square 90-degree frustum, row-major).
            const float invTan = 1.0f; // tan(45 deg) == 1, i.e. 90 degree FOV to cover one cube face.
            const float n = nearZ, f = farZ;
            const float proj[16] = {
                invTan, 0.0f,   0.0f,          0.0f,
                0.0f,   invTan, 0.0f,          0.0f,
                0.0f,   0.0f,   f / (f - n),   1.0f,
                0.0f,   0.0f,  -n*f / (f - n), 0.0f,
            };

            Mul4x4(view, proj, outVP);
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
