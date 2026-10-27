#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>

// Small header-only math kit for the CPU animation runtime (Source/Renderer/Animation).
// Conventions, shared by every file in this folder:
//   - Quat is (x, y, z, w), Hamilton product, unit length. QuatMul(a, b) applies b first, then a.
//   - Mat4 is column-major, m[col * 4 + row], translation in m[12..14]. This is the layout that
//     AnimationController::TrsToMatrix and Skeleton::inverseBindPose use.
//   - "Scaled angle axis" is axis * angle (radians), i.e. 2 * log(q) (Holden, "Spring-It-On").
namespace SasamiEngine::Render::Anim
{
    constexpr float ANIM_PI = 3.14159265358979323846f;
    constexpr float ANIM_EPS = 1e-6f;

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Quat
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    // Parent-relative joint transform. Scale is applied first, then rotation, then translation.
    struct Transform
    {
        Vec3 t{};
        Quat r{};
        Vec3 s{ 1.0f, 1.0f, 1.0f };
    };

    struct Mat4
    {
        float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };

    // ---- Vec3 ---------------------------------------------------------------------------------
    inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 operator-(const Vec3& a) { return { -a.x, -a.y, -a.z }; }
    inline Vec3 operator*(const Vec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline Vec3 operator*(float s, const Vec3& a) { return { a.x * s, a.y * s, a.z * s }; }
    inline Vec3& operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
    inline Vec3& operator-=(Vec3& a, const Vec3& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }

    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    inline float LengthSq(const Vec3& a) { return Dot(a, a); }
    inline float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    // Returns `fallback` for a (near) zero vector instead of producing NaN.
    inline Vec3 Normalize(const Vec3& a, const Vec3& fallback = { 0.0f, 0.0f, 0.0f })
    {
        const float len = Length(a);
        return len > ANIM_EPS ? a * (1.0f / len) : fallback;
    }
    inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
    inline Vec3 MulComponents(const Vec3& a, const Vec3& b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }

    // ---- Quat ---------------------------------------------------------------------------------
    inline Quat QuatMul(const Quat& a, const Quat& b)
    {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }
    inline Quat QuatConjugate(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
    // Inverse of a unit quaternion.
    inline Quat QuatInverse(const Quat& q) { return QuatConjugate(q); }
    inline float QuatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    inline Quat QuatNormalize(const Quat& q)
    {
        const float len = std::sqrt(QuatDot(q, q));
        if (len <= ANIM_EPS) {
            return Quat{};
        }
        const float inv = 1.0f / len;
        return { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
    }
    inline Quat QuatNegate(const Quat& q) { return { -q.x, -q.y, -q.z, -q.w }; }
    // Picks the hemisphere with w >= 0 (same rotation, shortest-path representative).
    inline Quat QuatAbs(const Quat& q) { return q.w < 0.0f ? QuatNegate(q) : q; }

    inline Vec3 QuatRotate(const Quat& q, const Vec3& v)
    {
        const Vec3 u{ q.x, q.y, q.z };
        const Vec3 t = Cross(u, v) * 2.0f;
        return v + t * q.w + Cross(u, t);
    }

    // `axis` must be unit length.
    inline Quat QuatFromAxisAngle(const Vec3& axis, float angle)
    {
        const float h = 0.5f * angle;
        const float s = std::sin(h);
        return { axis.x * s, axis.y * s, axis.z * s, std::cos(h) };
    }

    // Rotation about the unit `axis` by the angle whose cosine is `cosAngle` (ozz FromAxisCosAngle).
    inline Quat QuatFromAxisCosAngle(const Vec3& axis, float cosAngle)
    {
        const float c = std::clamp(cosAngle, -1.0f, 1.0f);
        const float halfCos = std::sqrt((std::max)(0.0f, 0.5f * (1.0f + c)));
        const float halfSin = std::sqrt((std::max)(0.0f, 0.5f * (1.0f - c)));
        return { axis.x * halfSin, axis.y * halfSin, axis.z * halfSin, halfCos };
    }

    // Shortest rotation taking direction `from` to direction `to`. Neither needs to be unit length.
    inline Quat QuatFromTo(const Vec3& from, const Vec3& to)
    {
        const Vec3 a = Normalize(from, { 0.0f, 0.0f, 1.0f });
        const Vec3 b = Normalize(to, { 0.0f, 0.0f, 1.0f });
        const float d = Dot(a, b);
        if (d < -0.999999f) {
            Vec3 axis = Cross({ 1.0f, 0.0f, 0.0f }, a);
            if (LengthSq(axis) < 1e-8f) {
                axis = Cross({ 0.0f, 1.0f, 0.0f }, a);
            }
            return QuatFromAxisAngle(Normalize(axis, { 0.0f, 1.0f, 0.0f }), ANIM_PI);
        }
        const Vec3 c = Cross(a, b);
        return QuatNormalize({ c.x, c.y, c.z, 1.0f + d });
    }

    // Normalized lerp along the shortest path.
    inline Quat QuatNlerp(const Quat& a, const Quat& b, float t)
    {
        const Quat bb = QuatDot(a, b) < 0.0f ? QuatNegate(b) : b;
        return QuatNormalize({
            a.x + (bb.x - a.x) * t,
            a.y + (bb.y - a.y) * t,
            a.z + (bb.z - a.z) * t,
            a.w + (bb.w - a.w) * t,
        });
    }

    inline Quat QuatSlerp(const Quat& a, const Quat& b, float t)
    {
        float d = QuatDot(a, b);
        Quat bb = b;
        if (d < 0.0f) {
            bb = QuatNegate(b);
            d = -d;
        }
        if (d > 0.9995f) {
            return QuatNlerp(a, bb, t);
        }
        const float theta0 = std::acos(d);
        const float theta = theta0 * t;
        const float sin0 = std::sin(theta0);
        const float s1 = std::sin(theta) / sin0;
        const float s0 = std::cos(theta) - d * s1;
        return { s0 * a.x + s1 * bb.x, s0 * a.y + s1 * bb.y, s0 * a.z + s1 * bb.z, s0 * a.w + s1 * bb.w };
    }

    // axis * angle. The caller usually passes QuatAbs(q) to get the short-way angle in [0, pi].
    inline Vec3 QuatToScaledAngleAxis(const Quat& q)
    {
        const Vec3 v{ q.x, q.y, q.z };
        const float len = Length(v);
        if (len < ANIM_EPS) {
            return v * 2.0f;
        }
        // atan2, not acos(w): near w = 1 a float acos cannot resolve half-angles below ~3e-4,
        // which inflated small rotations and stalled decaying springs (inertialization offsets).
        const float halfAngle = std::atan2(len, q.w);
        return v * (2.0f * halfAngle / len);
    }

    inline Quat QuatFromScaledAngleAxis(const Vec3& v)
    {
        const Vec3 half = v * 0.5f;
        const float halfAngle = Length(half);
        if (halfAngle < ANIM_EPS) {
            return QuatNormalize({ half.x, half.y, half.z, 1.0f });
        }
        const float s = std::sin(halfAngle) / halfAngle;
        return { half.x * s, half.y * s, half.z * s, std::cos(halfAngle) };
    }

    // Rotation about +Y (yaw). The model forward axis is +Z, so yaw = atan2(dir.x, dir.z).
    inline Quat QuatFromYaw(float yaw) { return QuatFromAxisAngle({ 0.0f, 1.0f, 0.0f }, yaw); }
    // Heading of the rotated +Z axis projected on the XZ plane. 0 when the projection degenerates.
    inline float QuatYaw(const Quat& q)
    {
        const Vec3 f = QuatRotate(q, { 0.0f, 0.0f, 1.0f });
        if (f.x * f.x + f.z * f.z < 1e-12f) {
            return 0.0f;
        }
        return std::atan2(f.x, f.z);
    }
    // Wraps an angle into (-pi, pi].
    inline float WrapAngle(float a)
    {
        a = std::fmod(a + ANIM_PI, 2.0f * ANIM_PI);
        if (a <= 0.0f) {
            a += 2.0f * ANIM_PI;
        }
        return a - ANIM_PI;
    }

    // ---- Transform ----------------------------------------------------------------------------
    inline Vec3 TransformPoint(const Transform& x, const Vec3& p) { return x.t + QuatRotate(x.r, MulComponents(x.s, p)); }

    // ---- Mat4 ---------------------------------------------------------------------------------
    inline Mat4 Mat4FromTransform(const Transform& x)
    {
        const Quat& q = x.r;
        const float x2 = q.x * q.x, y2 = q.y * q.y, z2 = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 o;
        o.m[0] = (1.0f - 2.0f * (y2 + z2)) * x.s.x;
        o.m[1] = (2.0f * (xy + wz)) * x.s.x;
        o.m[2] = (2.0f * (xz - wy)) * x.s.x;
        o.m[3] = 0.0f;
        o.m[4] = (2.0f * (xy - wz)) * x.s.y;
        o.m[5] = (1.0f - 2.0f * (x2 + z2)) * x.s.y;
        o.m[6] = (2.0f * (yz + wx)) * x.s.y;
        o.m[7] = 0.0f;
        o.m[8] = (2.0f * (xz + wy)) * x.s.z;
        o.m[9] = (2.0f * (yz - wx)) * x.s.z;
        o.m[10] = (1.0f - 2.0f * (x2 + y2)) * x.s.z;
        o.m[11] = 0.0f;
        o.m[12] = x.t.x;
        o.m[13] = x.t.y;
        o.m[14] = x.t.z;
        o.m[15] = 1.0f;
        return o;
    }

    inline Mat4 Mat4FromColumnMajor(const float* src)
    {
        Mat4 o;
        std::memcpy(o.m, src, sizeof(o.m));
        return o;
    }

    // out = a * b (b applied first).
    inline Mat4 Mat4Mul(const Mat4& a, const Mat4& b)
    {
        Mat4 o;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a.m[k * 4 + row] * b.m[col * 4 + k];
                }
                o.m[col * 4 + row] = sum;
            }
        }
        return o;
    }

    inline Vec3 Mat4Translation(const Mat4& a) { return { a.m[12], a.m[13], a.m[14] }; }
    inline Vec3 Mat4Column(const Mat4& a, int col) { return { a.m[col * 4 + 0], a.m[col * 4 + 1], a.m[col * 4 + 2] }; }

    inline Vec3 Mat4TransformPoint(const Mat4& a, const Vec3& p)
    {
        return {
            a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
            a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14],
        };
    }

    inline Vec3 Mat4TransformVector(const Mat4& a, const Vec3& v)
    {
        return {
            a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z,
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z,
        };
    }

    // Inverse of an affine matrix (last row 0 0 0 1). A singular matrix returns all zeros, which
    // turns every IK correction computed from it into identity (same policy as ozz).
    inline Mat4 Mat4InverseAffine(const Mat4& a)
    {
        // 3x3 block, r[row][col].
        const float a00 = a.m[0], a01 = a.m[4], a02 = a.m[8];
        const float a10 = a.m[1], a11 = a.m[5], a12 = a.m[9];
        const float a20 = a.m[2], a21 = a.m[6], a22 = a.m[10];
        const float c00 = a11 * a22 - a12 * a21;
        const float c01 = a12 * a20 - a10 * a22;
        const float c02 = a10 * a21 - a11 * a20;
        const float det = a00 * c00 + a01 * c01 + a02 * c02;
        Mat4 o;
        if (std::fabs(det) < 1e-12f) {
            std::memset(o.m, 0, sizeof(o.m));
            return o;
        }
        const float id = 1.0f / det;
        // inverse[row][col] = cofactor[col][row] / det
        const float i00 = c00 * id;
        const float i01 = (a02 * a21 - a01 * a22) * id;
        const float i02 = (a01 * a12 - a02 * a11) * id;
        const float i10 = c01 * id;
        const float i11 = (a00 * a22 - a02 * a20) * id;
        const float i12 = (a02 * a10 - a00 * a12) * id;
        const float i20 = c02 * id;
        const float i21 = (a01 * a20 - a00 * a21) * id;
        const float i22 = (a00 * a11 - a01 * a10) * id;
        o.m[0] = i00; o.m[4] = i01; o.m[8] = i02;
        o.m[1] = i10; o.m[5] = i11; o.m[9] = i12;
        o.m[2] = i20; o.m[6] = i21; o.m[10] = i22;
        o.m[3] = 0.0f; o.m[7] = 0.0f; o.m[11] = 0.0f;
        const float tx = a.m[12], ty = a.m[13], tz = a.m[14];
        o.m[12] = -(i00 * tx + i01 * ty + i02 * tz);
        o.m[13] = -(i10 * tx + i11 * ty + i12 * tz);
        o.m[14] = -(i20 * tx + i21 * ty + i22 * tz);
        o.m[15] = 1.0f;
        return o;
    }

    // Rotation of a pure rotation matrix (columns must be orthonormal). Shepperd's method.
    inline Quat QuatFromRotationMat4(const Mat4& a)
    {
        const float m00 = a.m[0], m11 = a.m[5], m22 = a.m[10];
        const float m01 = a.m[4], m02 = a.m[8], m10 = a.m[1];
        const float m12 = a.m[9], m20 = a.m[2], m21 = a.m[6];
        const float trace = m00 + m11 + m22;
        Quat q;
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
        } else if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
        } else if (m11 > m22) {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
        } else {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
        }
        return QuatNormalize(q);
    }

    // TRS decomposition of an affine matrix without shear. Negative determinant flips scale.x.
    inline Transform Mat4ToTransform(const Mat4& a)
    {
        Transform x;
        x.t = Mat4Translation(a);
        Vec3 c0 = Mat4Column(a, 0), c1 = Mat4Column(a, 1), c2 = Mat4Column(a, 2);
        x.s = { Length(c0), Length(c1), Length(c2) };
        if (Dot(Cross(c0, c1), c2) < 0.0f) {
            x.s.x = -x.s.x;
        }
        Mat4 r;
        const Vec3 n0 = x.s.x != 0.0f ? c0 * (1.0f / x.s.x) : Vec3{ 1.0f, 0.0f, 0.0f };
        const Vec3 n1 = x.s.y != 0.0f ? c1 * (1.0f / x.s.y) : Vec3{ 0.0f, 1.0f, 0.0f };
        const Vec3 n2 = x.s.z != 0.0f ? c2 * (1.0f / x.s.z) : Vec3{ 0.0f, 0.0f, 1.0f };
        r.m[0] = n0.x; r.m[1] = n0.y; r.m[2] = n0.z;
        r.m[4] = n1.x; r.m[5] = n1.y; r.m[6] = n1.z;
        r.m[8] = n2.x; r.m[9] = n2.y; r.m[10] = n2.z;
        x.r = QuatFromRotationMat4(r);
        return x;
    }
}
