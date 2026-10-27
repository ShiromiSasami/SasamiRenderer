#include "Renderer/Animation/PoseModelSpace.h"

#include <algorithm>
#include <cstring>

namespace SasamiEngine::Render::Anim
{
    namespace
    {
        uint32_t ClampBoneCount(const Skeleton& skeleton)
        {
            return std::min(skeleton.boneCount, Skeleton::kMaxBones);
        }

        bool HasParent(const Skeleton& skeleton, uint32_t i)
        {
            const int32_t p = skeleton.parentIndex[i];
            return p >= 0 && static_cast<uint32_t>(p) < i;
        }
    }

    void LocalToModel(const Skeleton& skeleton, const LocalPose& local, ModelPose& outModel)
    {
        const uint32_t count = ClampBoneCount(skeleton);
        outModel.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const Mat4 m = i < local.size() ? Mat4FromTransform(local[i]) : Mat4{};
            outModel[i] = HasParent(skeleton, i) ? Mat4Mul(outModel[skeleton.parentIndex[i]], m) : m;
        }
    }

    void ModelToSkinMatrices(const Skeleton& skeleton, const ModelPose& model, float* outMatrices)
    {
        const uint32_t count = std::min<uint32_t>(ClampBoneCount(skeleton), static_cast<uint32_t>(model.size()));
        for (uint32_t i = 0; i < count; ++i)
        {
            const Mat4 skin = Mat4Mul(model[i], Mat4FromColumnMajor(skeleton.inverseBindPose[i]));
            std::memcpy(outMatrices + i * 16, skin.m, sizeof(skin.m));
        }

        const Mat4 identity{};
        for (uint32_t i = count; i < Skeleton::kMaxBones; ++i)
            std::memcpy(outMatrices + i * 16, identity.m, sizeof(identity.m));
    }

    void ModelToLocal(const Skeleton& skeleton, const ModelPose& model, LocalPose& outLocal)
    {
        const uint32_t count = ClampBoneCount(skeleton);
        outLocal.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (i >= model.size())
            {
                outLocal[i] = Transform{};
                continue;
            }

            if (HasParent(skeleton, i))
                outLocal[i] = Mat4ToTransform(Mat4Mul(Mat4InverseAffine(model[skeleton.parentIndex[i]]), model[i]));
            else
                outLocal[i] = Mat4ToTransform(model[i]);
        }
    }
}
