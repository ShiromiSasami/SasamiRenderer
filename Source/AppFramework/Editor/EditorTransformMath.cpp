#include "AppFramework/Editor/EditorTransformMath.h"

#include <cmath>
#include <algorithm>

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;
        constexpr float MIN_SCALE = 1e-8f;
        constexpr float GIMBAL_COS = 1e-6f;
        constexpr float TOLERANCE = 1e-4f;

        float RowLength(const float m[16], int row)
        {
            const float* r = m + row * 4;
            return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        }

        float MaxMatrixDiff(const float a[16], const float b[16])
        {
            float maxDiff = 0.0f;
            for (int i = 0; i < 16; ++i) {
                maxDiff = (std::max)(maxDiff, std::fabs(a[i] - b[i]));
            }
            return maxDiff;
        }
    }

    void ComposeTrs(const EditorTrs& trs, float out[16])
    {
        const float cx = std::cos(trs.rotation.pitch);
        const float sx = std::sin(trs.rotation.pitch);
        const float cy = std::cos(trs.rotation.yaw);
        const float sy = std::sin(trs.rotation.yaw);
        const float cz = std::cos(trs.rotation.roll);
        const float sz = std::sin(trs.rotation.roll);

        // R = Rz * Rx * Ry, row vectors.
        const float r[3][3] = {
            { cz * cy + sz * sx * sy, sz * cx, -cz * sy + sz * sx * cy },
            { -sz * cy + cz * sx * sy, cz * cx, sz * sy + cz * sx * cy },
            { cx * sy, -sx, cx * cy },
        };
        const float s[3] = { trs.scale.x, trs.scale.y, trs.scale.z };

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                out[i * 4 + j] = r[i][j] * s[i];
            }
            out[i * 4 + 3] = 0.0f;
        }
        out[12] = trs.translation.x;
        out[13] = trs.translation.y;
        out[14] = trs.translation.z;
        out[15] = 1.0f;
    }

    EditorTrs DecomposeTrs(const float m[16])
    {
        EditorTrs trs;
        trs.translation = Vector3{ m[12], m[13], m[14] };

        float scale[3] = { RowLength(m, 0), RowLength(m, 1), RowLength(m, 2) };

        const float det =
            m[0] * (m[5] * m[10] - m[6] * m[9]) -
            m[1] * (m[4] * m[10] - m[6] * m[8]) +
            m[2] * (m[4] * m[9] - m[5] * m[8]);
        if (det < 0.0f) {
            scale[0] = -scale[0];
        }

        float r[3][3];
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(scale[i]) < MIN_SCALE) {
                scale[i] = 0.0f;
                for (int j = 0; j < 3; ++j) {
                    r[i][j] = (i == j) ? 1.0f : 0.0f;
                }
            } else {
                for (int j = 0; j < 3; ++j) {
                    r[i][j] = m[i * 4 + j] / scale[i];
                }
            }
        }
        trs.scale = Vector3{ scale[0], scale[1], scale[2] };

        const float pitch = std::asin((std::min)((std::max)(-r[2][1], -1.0f), 1.0f));
        float yaw;
        float roll;
        if (std::cos(pitch) > GIMBAL_COS) {
            yaw = std::atan2(r[2][0], r[2][2]);
            roll = std::atan2(r[0][1], r[1][1]);
        } else {
            roll = 0.0f;
            yaw = std::atan2(-r[0][2], r[0][0]);
        }
        trs.rotation = Rotation3(pitch, yaw, roll);
        return trs;
    }

    int RunEditorTransformSelfTest(int& checks)
    {
        int failures = 0;
        checks = 0;
        auto check = [&](bool ok) {
            ++checks;
            if (!ok) {
                ++failures;
            }
        };

        {
            const float identity[16] = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            };
            const EditorTrs trs = DecomposeTrs(identity);
            check(std::fabs(trs.rotation.pitch) < TOLERANCE &&
                  std::fabs(trs.rotation.yaw) < TOLERANCE &&
                  std::fabs(trs.rotation.roll) < TOLERANCE);
            check(std::fabs(trs.scale.x - 1.0f) < TOLERANCE &&
                  std::fabs(trs.scale.y - 1.0f) < TOLERANCE &&
                  std::fabs(trs.scale.z - 1.0f) < TOLERANCE);
        }

        const float deg = PI / 180.0f;
        const float angles[8][3] = {
            { 10.0f, 20.0f, 30.0f },
            { -45.0f, 120.0f, -60.0f },
            { 70.0f, -150.0f, 100.0f },
            { -79.0f, 5.0f, 165.0f },
            { 0.0f, 90.0f, 0.0f },
            { 30.0f, -90.0f, -120.0f },
            { 55.0f, 169.0f, -169.0f },
            { -10.0f, -30.0f, 0.0f },
        };
        const float scales[8][3] = {
            { 0.5f, 2.0f, 3.0f },
            { 1.0f, 1.0f, 1.0f },
            { 2.0f, 0.25f, 1.5f },
            { 3.0f, 3.0f, 0.5f },
            { 0.5f, 2.0f, 3.0f },
            { 1.0f, 4.0f, 2.0f },
            { 0.1f, 0.2f, 0.3f },
            { 5.0f, 1.0f, 1.0f },
        };
        for (int i = 0; i < 8; ++i) {
            EditorTrs in;
            in.translation = Vector3{ 1.0f + i, -2.0f * i, 0.5f - i };
            in.rotation = Rotation3(angles[i][0] * deg, angles[i][1] * deg, angles[i][2] * deg);
            in.scale = Vector3{ scales[i][0], scales[i][1], scales[i][2] };

            float a[16];
            float b[16];
            ComposeTrs(in, a);
            const EditorTrs out = DecomposeTrs(a);
            ComposeTrs(out, b);

            check(MaxMatrixDiff(a, b) < TOLERANCE);
            check(std::fabs(out.rotation.pitch - in.rotation.pitch) < TOLERANCE &&
                  std::fabs(out.rotation.yaw - in.rotation.yaw) < TOLERANCE &&
                  std::fabs(out.rotation.roll - in.rotation.roll) < TOLERANCE);
        }

        {
            EditorTrs yawOnly;
            yawOnly.rotation = Rotation3(0.0f, 90.0f * deg, 0.0f);
            float m[16];
            ComposeTrs(yawOnly, m);
            check(std::fabs(m[0]) < TOLERANCE && std::fabs(m[1]) < TOLERANCE &&
                  std::fabs(m[2] + 1.0f) < TOLERANCE);
        }

        {
            EditorTrs gimbal;
            gimbal.translation = Vector3{ 3.0f, 4.0f, 5.0f };
            gimbal.rotation = Rotation3(90.0f * deg, 25.0f * deg, 40.0f * deg);
            gimbal.scale = Vector3{ 2.0f, 1.0f, 0.5f };
            float a[16];
            float b[16];
            ComposeTrs(gimbal, a);
            ComposeTrs(DecomposeTrs(a), b);
            check(MaxMatrixDiff(a, b) < TOLERANCE);
        }

        return failures;
    }
}
