#pragma once

#include <cassert>

#include "Object/SObject.h"
#include "Object/MeshComponent.h"

namespace SasamiRenderer
{
    class StaticModel : public SObject
    {
    public:
        StaticModel() { AddComponent<MeshComponent>(); }

        using ModelFormat = MeshComponent::ModelFormat;

        bool LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale = 1.0f);
        std::vector<RenderProxy> BuildRenderProxies() const;
        void AddStaticMesh(Mesh mesh,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "");
        void ClearModel();
        void SetTranslation(float x, float y, float z);

        MeshComponent& MeshComponentRef()
        {
            MeshComponent* component = GetComponent<MeshComponent>();
            assert(component && "StaticModel requires MeshComponent.");
            return *component;
        }
        const MeshComponent& MeshComponentRef() const
        {
            const MeshComponent* component = GetComponent<MeshComponent>();
            assert(component && "StaticModel requires MeshComponent.");
            return *component;
        }
    };
}
