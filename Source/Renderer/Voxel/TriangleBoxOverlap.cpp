#include "Renderer/Voxel/TriangleBoxOverlap.h"

#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        bool SeparatedOnAxis(const float axis[3], const float h[3],
                             const float p0[3], const float p1[3], const float p2[3])
        {
            const float d0 = axis[0] * p0[0] + axis[1] * p0[1] + axis[2] * p0[2];
            const float d1 = axis[0] * p1[0] + axis[1] * p1[1] + axis[2] * p1[2];
            const float d2 = axis[0] * p2[0] + axis[1] * p2[1] + axis[2] * p2[2];

            float minD = d0 < d1 ? d0 : d1;
            minD = minD < d2 ? minD : d2;
            float maxD = d0 > d1 ? d0 : d1;
            maxD = maxD > d2 ? maxD : d2;

            const float r = h[0] * std::fabs(axis[0]) + h[1] * std::fabs(axis[1]) + h[2] * std::fabs(axis[2]);
            return minD > r || maxD < -r;
        }
    }

    bool TriangleOverlapsBox(const float boxCenter[3],
                             const float boxHalfSize[3],
                             const float v0[3],
                             const float v1[3],
                             const float v2[3])
    {
        float p0[3];
        float p1[3];
        float p2[3];
        for (int i = 0; i < 3; ++i)
        {
            p0[i] = v0[i] - boxCenter[i];
            p1[i] = v1[i] - boxCenter[i];
            p2[i] = v2[i] - boxCenter[i];
        }

        const float e0[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const float e1[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
        const float e2[3] = { p0[0] - p2[0], p0[1] - p2[1], p0[2] - p2[2] };
        const float* edges[3] = { e0, e1, e2 };

        for (int e = 0; e < 3; ++e)
        {
            const float* edge = edges[e];
            const float axes[3][3] = {
                { 0.0f, -edge[2], edge[1] },
                { edge[2], 0.0f, -edge[0] },
                { -edge[1], edge[0], 0.0f },
            };
            for (int a = 0; a < 3; ++a)
            {
                if (SeparatedOnAxis(axes[a], boxHalfSize, p0, p1, p2))
                {
                    return false;
                }
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            float minV = p0[i] < p1[i] ? p0[i] : p1[i];
            minV = minV < p2[i] ? minV : p2[i];
            float maxV = p0[i] > p1[i] ? p0[i] : p1[i];
            maxV = maxV > p2[i] ? maxV : p2[i];
            if (minV > boxHalfSize[i] || maxV < -boxHalfSize[i])
            {
                return false;
            }
        }

        const float n[3] = {
            e0[1] * e1[2] - e0[2] * e1[1],
            e0[2] * e1[0] - e0[0] * e1[2],
            e0[0] * e1[1] - e0[1] * e1[0],
        };
        if (n[0] != 0.0f || n[1] != 0.0f || n[2] != 0.0f)
        {
            const float r = boxHalfSize[0] * std::fabs(n[0])
                          + boxHalfSize[1] * std::fabs(n[1])
                          + boxHalfSize[2] * std::fabs(n[2]);
            const float d = n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2];
            if (std::fabs(d) > r)
            {
                return false;
            }
        }

        return true;
    }
}
