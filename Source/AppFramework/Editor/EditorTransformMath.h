#pragma once

#include "Foundation/Math/Rotation3.h"
#include "Foundation/Math/Vector3.h"

namespace SasamiEngine::App
{
    // Translation / rotation (radians) / scale of a row-major, row-vector model matrix.
    struct EditorTrs
    {
        Vector3 translation{ 0.0f, 0.0f, 0.0f };
        Rotation3 rotation{};
        Vector3 scale{ 1.0f, 1.0f, 1.0f };
    };

    // out = S * R * T with R = Rz(roll) * Rx(pitch) * Ry(yaw) (row vectors: roll first, then pitch,
    // then yaw), the order of DirectX::XMMatrixRotationRollPitchYaw. Translation goes to out[12..14].
    void ComposeTrs(const EditorTrs& trs, float out[16]);

    // Inverse of ComposeTrs for matrices without shear. Scale is the row lengths (all positive; a
    // mirrored matrix gets a negative x scale). At pitch = +-90 deg roll is folded into yaw.
    EditorTrs DecomposeTrs(const float m[16]);

    // Round-trip checks of ComposeTrs/DecomposeTrs; returns the number of failed checks and the
    // number run in `checks`.
    int RunEditorTransformSelfTest(int& checks);
}
