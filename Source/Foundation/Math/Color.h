#pragma once

namespace SasamiEngine::Foundation
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

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
