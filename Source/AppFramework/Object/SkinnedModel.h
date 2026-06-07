#pragma once

#include <cassert>

#include "Component/SkinnedMeshComponent.h"
#include "Object/SObject.h"

namespace SasamiRenderer
{
    class SkinnedModel : public SObject
    {
    public:
        SkinnedModel() { AddComponent<SkinnedMeshComponent>(); }

        using ModelFormat = SkinnedMeshComponent::ModelFormat;

        bool LoadModel(const std::string& assetPath, ModelFormat format = ModelFormat::Gltf);
        void UpdateAnimation(float deltaTime);
        std::vector<SkinnedRenderProxy> BuildRenderProxies();
        void PlayAnimation(int animationIndex, bool loop = true);
        bool HasAnimation() const;
        int CurrentAnimation() const;
        void SetTranslation(float x, float y, float z);
        void ClearModel();

        SkinnedMeshComponent& SkinnedMeshComponentRef()
        {
            SkinnedMeshComponent* component = GetComponent<SkinnedMeshComponent>();
            assert(component && "SkinnedModel requires SkinnedMeshComponent.");
            return *component;
        }

        const SkinnedMeshComponent& SkinnedMeshComponentRef() const
        {
            const SkinnedMeshComponent* component = GetComponent<SkinnedMeshComponent>();
            assert(component && "SkinnedModel requires SkinnedMeshComponent.");
            return *component;
        }
    };
}
