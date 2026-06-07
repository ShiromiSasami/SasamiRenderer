#include "Object/SkinnedModel.h"

namespace SasamiRenderer
{
    bool SkinnedModel::LoadModel(const std::string& assetPath, ModelFormat format)
    {
        return SkinnedMeshComponentRef().LoadModel(assetPath, format);
    }

    void SkinnedModel::UpdateAnimation(float deltaTime)
    {
        SkinnedMeshComponentRef().UpdateAnimation(deltaTime);
    }

    std::vector<SkinnedRenderProxy> SkinnedModel::BuildRenderProxies()
    {
        return SkinnedMeshComponentRef().BuildRenderProxies();
    }

    void SkinnedModel::PlayAnimation(int animationIndex, bool loop)
    {
        SkinnedMeshComponentRef().PlayAnimation(animationIndex, loop);
    }

    bool SkinnedModel::HasAnimation() const
    {
        return SkinnedMeshComponentRef().HasAnimation();
    }

    int SkinnedModel::CurrentAnimation() const
    {
        return SkinnedMeshComponentRef().CurrentAnimation();
    }

    void SkinnedModel::SetTranslation(float x, float y, float z)
    {
        SkinnedMeshComponentRef().SetTranslation(x, y, z);
    }

    void SkinnedModel::ClearModel()
    {
        SkinnedMeshComponentRef().Clear();
    }
}
