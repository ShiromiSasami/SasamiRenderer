#pragma once

#include <cassert>
#include <cstdint>

#include "Object/SObject.h"
#include "Object/MeshComponent.h"

namespace SasamiRenderer
{
    class StaticModel : public SObject
    {
    public:
        StaticModel() { AddComponent<MeshComponent>(); }

        using ModelFormat = MeshComponent::ModelFormat;

        struct BoxDesc
        {
            float width = 1.0f;
            float height = 1.0f;
            float depth = 1.0f;
            SurfaceMaterial material{};
            std::string albedoTexturePath;
            std::string occlusionTexturePath;
        };

        struct SphereDesc
        {
            float radius = 0.5f;
            uint32_t slices = 32u;
            uint32_t stacks = 16u;
            SurfaceMaterial material{};
            std::string albedoTexturePath;
            std::string occlusionTexturePath;
        };

        bool LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale = 1.0f);
        std::vector<RenderProxy> BuildRenderProxies() const;
        void AddStaticMesh(Mesh mesh,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "");
        void AddStaticMesh(Mesh mesh,
                           const SurfaceMaterial& material,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "");
        void AddBox(const BoxDesc& desc = {});
        void AddSphere(const SphereDesc& desc = {});
        bool SetMaterial(size_t meshIndex, const SurfaceMaterial& material);
        const SurfaceMaterial* GetMaterial(size_t meshIndex) const;
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
