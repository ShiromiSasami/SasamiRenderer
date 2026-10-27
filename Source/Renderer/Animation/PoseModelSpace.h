#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"

namespace SasamiEngine::Render::Anim
{
    // model[i] = model[parent] * local[i] (local[i] for roots). Resizes `outModel` to boneCount.
    void LocalToModel(const Skeleton& skeleton, const LocalPose& local, ModelPose& outModel);

    // Skinning palette in the AnimationController::GetBoneMatrices layout:
    // Skeleton::kMaxBones * 16 floats, out[i] = model[i] * inverseBindPose[i], identity past boneCount.
    void ModelToSkinMatrices(const Skeleton& skeleton, const ModelPose& model, float* outMatrices);

    // Inverse of LocalToModel: local[i] = decompose(inverse(model[parent]) * model[i]).
    // Exact for poses without shear (non-uniform scale on a parent with a rotated child is lossy).
    void ModelToLocal(const Skeleton& skeleton, const ModelPose& model, LocalPose& outLocal);
}

namespace SasamiEngine { using namespace Render; }
