#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AppFramework/Component/IComponent.h"
#include "Renderer/Scene/AnimationController.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::App
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
        void SetUniformScale(float scale);
        // Row-major, row-vector component transform (translation in [12..14]).
        void SetModelMatrix(const float matrix[16]) { for (int i = 0; i < 16; ++i) m_model[i] = matrix[i]; }
        const float* GetModelMatrix() const { return m_model; }
        // Axis-aligned bounds of the bind-pose vertices in component space (before GetModelMatrix).
        // Animation is ignored, so a posed character can stick out of them. False when empty.
        bool ComputeLocalBounds(float outMin[3], float outMax[3]) const;
        const std::string& GetAssetPath() const { return m_debugAssetPath; }
        void Clear();

    private:
        struct SkinnedMeshSource
        {
            SkinnedMesh mesh;
            uint64_t meshId = 0;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            std::shared_ptr<const CpuTextureRgba8> normalTexture;
            std::shared_ptr<const CpuTextureRgba8> emissiveTexture;
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
namespace SasamiEngine { using namespace App; }
