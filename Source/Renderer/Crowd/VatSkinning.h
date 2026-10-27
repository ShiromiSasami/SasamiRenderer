#pragma once

#include "Renderer/Structures/SkinnedVertex.h"

namespace SasamiEngine::Render::Crowd
{
    // CPU mirror of SkinnedMesh_VS.hlsl SkinPosition/SkinNormal: 4-influence linear blend skinning in
    // model space. `boneMatrices` holds column-major float4x4 skinning matrices (global * inverseBind),
    // 16 floats each, as AnimationController::GetBoneMatrices writes them. The normal is normalized
    // (zero-length input stays zero). Bone indices are trusted to be < the matrix count.
    void SkinVertexCpu(const SkinnedVertex& vertex, const float* boneMatrices, float outPosition[3], float outNormal[3]);

    // Same blend, but each bone is a 3x4 affine row block (12 floats: three float4 rows, so that
    // p'_r = dot(row_r, float4(p, 1))), the layout VatBakedAnimation::boneRows stores per frame.
    void SkinVertexWithBoneRows(const SkinnedVertex& vertex, const float* boneRows, float outPosition[3], float outNormal[3]);

    // Converts one column-major float4x4 into the 3x4 row block above (drops the projective row).
    void ColumnMajorToBoneRows(const float* columnMajor16, float outRows12[12]);
}

namespace SasamiEngine { using namespace Render; }
