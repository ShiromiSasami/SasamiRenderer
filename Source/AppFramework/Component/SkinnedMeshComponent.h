#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AppFramework/Component/IComponent.h"
#include "Renderer/Scene/AnimationController.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
    class SkinnedMeshComponent : public IComponent
    {
    public:
        enum class ModelFormat
        {
            Gltf,
        };

        bool LoadModel(const std::string& assetPath, ModelFormat format = ModelFormat::Gltf);
        std::vector<SkinnedRenderProxy> BuildRenderProxies();
        void UpdateAnimation(float deltaTime);
        void PlayAnimation(int animationIndex, bool loop = true);
        bool HasAnimation() const;
        int CurrentAnimation() const;
        void SetTranslation(float x, float y, float z);
        void Clear();

    private:
        struct SkinnedMeshSource
        {
            SkinnedMesh mesh;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            SurfaceMaterial material;
            bool transparent = false;
        };

        std::vector<SkinnedMeshSource> m_meshes;
        std::shared_ptr<Skeleton> m_skeleton;
        AnimationController m_animationController;
        std::string m_debugAssetPath;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
