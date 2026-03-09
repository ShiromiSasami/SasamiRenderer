#pragma once

#include <cstddef>

namespace SasamiRenderer
{
    namespace LightCBLayout
    {
        constexpr size_t kDiffuseShCoefficientCount = 9u;

        struct LightCBData
        {
            float lightVP[16];
            float dirDir[4];
            float dirColor[4];
            float lightCounts[4];
            float cameraPos[4];
            float iblParams[4];
            float debugParams[4];
            float diffuseSh[kDiffuseShCoefficientCount][4];
        };
    }
}
