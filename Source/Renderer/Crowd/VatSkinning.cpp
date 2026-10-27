#include "Renderer/Crowd/VatSkinning.h"

#include <cmath>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        void NormalizeOrZero(float n[3])
        {
            const float lenSq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            if (lenSq > 1e-20f)
            {
                const float invLen = 1.0f / std::sqrt(lenSq);
                n[0] *= invLen;
                n[1] *= invLen;
                n[2] *= invLen;
            }
            else
            {
                n[0] = n[1] = n[2] = 0.0f;
            }
        }
    }

    void SkinVertexCpu(const SkinnedVertex& vertex, const float* boneMatrices, float outPosition[3], float outNormal[3])
    {
        const float* p = vertex.position;
        const float* n = vertex.normal;
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float nrm[3] = { 0.0f, 0.0f, 0.0f };

        for (int i = 0; i < 4; ++i)
        {
            const float  w = vertex.boneWeights[i];
            const float* a = boneMatrices + static_cast<size_t>(vertex.boneIndices[i]) * 16;
            for (int r = 0; r < 3; ++r)
            {
                const float tp = a[0 * 4 + r] * p[0] + a[1 * 4 + r] * p[1] + a[2 * 4 + r] * p[2] + a[12 + r];
                const float tn = a[0 * 4 + r] * n[0] + a[1 * 4 + r] * n[1] + a[2 * 4 + r] * n[2];
                pos[r] += w * tp;
                nrm[r] += w * tn;
            }
        }

        NormalizeOrZero(nrm);
        for (int r = 0; r < 3; ++r)
        {
            outPosition[r] = pos[r];
            outNormal[r]   = nrm[r];
        }
    }

    void SkinVertexWithBoneRows(const SkinnedVertex& vertex, const float* boneRows, float outPosition[3], float outNormal[3])
    {
        const float* p = vertex.position;
        const float* n = vertex.normal;
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        float nrm[3] = { 0.0f, 0.0f, 0.0f };

        for (int i = 0; i < 4; ++i)
        {
            const float  w    = vertex.boneWeights[i];
            const float* rows = boneRows + static_cast<size_t>(vertex.boneIndices[i]) * 12;
            for (int r = 0; r < 3; ++r)
            {
                const float* row = rows + r * 4;
                const float  tp  = row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3];
                const float  tn  = row[0] * n[0] + row[1] * n[1] + row[2] * n[2];
                pos[r] += w * tp;
                nrm[r] += w * tn;
            }
        }

        NormalizeOrZero(nrm);
        for (int r = 0; r < 3; ++r)
        {
            outPosition[r] = pos[r];
            outNormal[r]   = nrm[r];
        }
    }

    void ColumnMajorToBoneRows(const float* columnMajor16, float outRows12[12])
    {
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                outRows12[r * 4 + c] = columnMajor16[c * 4 + r];
            }
        }
    }
}
