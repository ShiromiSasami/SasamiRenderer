#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <memory>

#include "AppFramework/Component/IComponent.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
    class MeshComponent : public IComponent
    {
    public:
        enum class ModelFormat
        {
            Obj,
            Gltf,
            GltfStatic = Gltf, // Backward compatibility alias
        };

        bool LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale = 1.0f);
        void AddStaticMesh(Mesh mesh,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "");
        void AddStaticMesh(Mesh mesh,
                           const SurfaceMaterial& material,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "");
        std::vector<RenderProxy> BuildRenderProxies() const;
        bool SetMaterial(size_t meshIndex, const SurfaceMaterial& material);
        const SurfaceMaterial* GetMaterial(size_t meshIndex) const;

        void Clear();
        void SetTranslation(float x, float y, float z);

    private:
        struct StaticMeshSource
        {
            Mesh mesh;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            bool usesMetallicRoughnessTexture = false;
            SurfaceMaterial material;
            float localTransform[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        std::vector<StaticMeshSource> m_staticMeshes;
        std::string m_debugAssetPath;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
