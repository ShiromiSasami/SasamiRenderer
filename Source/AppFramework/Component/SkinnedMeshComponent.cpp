#include "Component/SkinnedMeshComponent.h"

#include <atomic>
#include <cstdint>
#include <utility>

#include "ApplicationResourcePaths.h"
#include "Component/CpuTextureLoader.h"
#include "Foundation/Math/MathUtil.h"
#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    bool SkinnedMeshComponent::LoadModel(const std::string& assetPath, ModelFormat format)
    {
        Clear();
        if (format != ModelFormat::Gltf) {
            return false;
        }

        const std::string fullPath = ApplicationResourcePaths::ResolveAssetPathString(assetPath);
        SkinnedModelData modelData;
        if (!LoadGLTFSkinned(fullPath, modelData)) {
            return false;
        }

        m_skeleton = std::make_shared<Skeleton>(modelData.skeleton);
        m_animationController.SetSkeleton(m_skeleton);
        for (auto& animation : modelData.animations) {
            m_animationController.AddAnimation(std::move(animation));
        }
        if (!modelData.animations.empty()) {
            m_animationController.PlayAnimation(0, true);
            m_animationController.Update(0.0f);
        }

        static std::atomic<uint64_t> sNextSkinnedMeshId{ 1 };

        m_meshes.reserve(modelData.meshes.size());
        for (size_t i = 0; i < modelData.meshes.size(); ++i) {
            SkinnedMeshSource source;
            source.mesh = std::move(modelData.meshes[i]);
            source.meshId = sNextSkinnedMeshId.fetch_add(1, std::memory_order_relaxed);
            if (i < modelData.albedoTexturePaths.size()) {
                source.albedoTexture = LoadCpuTextureFromPath(modelData.albedoTexturePaths[i]);
            }
            if (i < modelData.occlusionTexturePaths.size()) {
                source.occlusionTexture = LoadCpuTextureFromPath(modelData.occlusionTexturePaths[i]);
            }
            if (i < modelData.normalTexturePaths.size()) {
                source.normalTexture = LoadCpuTextureFromPath(modelData.normalTexturePaths[i]);
            }
            if (i < modelData.materials.size()) {
                source.material = modelData.materials[i];
            }
            source.transparent = IsTransparentMaterial(source.material);
            m_meshes.push_back(std::move(source));
        }

        m_debugAssetPath = assetPath;
        return !m_meshes.empty() && m_skeleton && m_skeleton->boneCount > 0;
    }

    std::vector<SkinnedRenderProxy> SkinnedMeshComponent::BuildRenderProxies()
    {
        std::vector<SkinnedRenderProxy> proxies;
        proxies.reserve(m_meshes.size());

        for (const auto& source : m_meshes) {
            SkinnedRenderProxy proxy;
            proxy.mesh = source.mesh;
            proxy.meshId = source.meshId;
            proxy.animController = &m_animationController;
            proxy.albedoTexture = source.albedoTexture;
            proxy.occlusionTexture = source.occlusionTexture;
            proxy.normalTexture = source.normalTexture;
            proxy.material = source.material;
            proxy.transparent = source.transparent;
            for (int i = 0; i < 16; ++i) {
                proxy.model[i] = m_model[i];
            }
            proxies.push_back(std::move(proxy));
        }

        return proxies;
    }

    void SkinnedMeshComponent::UpdateAnimation(float deltaTime)
    {
        m_animationController.Update(deltaTime);
    }

    void SkinnedMeshComponent::PlayAnimation(int animationIndex, bool loop)
    {
        m_animationController.PlayAnimation(animationIndex, loop);
        m_animationController.Update(0.0f);
    }

    bool SkinnedMeshComponent::HasAnimation() const
    {
        return m_animationController.HasAnimations();
    }

    int SkinnedMeshComponent::CurrentAnimation() const
    {
        return m_animationController.CurrentAnimation();
    }

    void SkinnedMeshComponent::SetTranslation(float x, float y, float z)
    {
        m_model[12] = x;
        m_model[13] = y;
        m_model[14] = z;
    }

    void SkinnedMeshComponent::SetUniformScale(float scale)
    {
        m_model[0]  = scale;
        m_model[5]  = scale;
        m_model[10] = scale;
    }

    void SkinnedMeshComponent::Clear()
    {
        m_meshes.clear();
        m_skeleton.reset();
        m_animationController = AnimationController{};
        m_debugAssetPath.clear();
        Math::Identity4x4(m_model);
    }
}
