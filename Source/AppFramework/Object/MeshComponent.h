#pragma once

#include <string>
#include <vector>
#include <memory>

#include "AppFramework/Component/IComponent.h"
#include "Renderer/Structures/Mesh.h"
#include "Renderer/Scene/RenderProxy.h"

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
        std::vector<RenderProxy> BuildRenderProxies() const;

        void Clear();
        void SetTranslation(float x, float y, float z);

    private:
        struct StaticMeshSource
        {
            Mesh mesh;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            float localTransform[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        std::vector<StaticMeshSource> m_staticMeshes;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
