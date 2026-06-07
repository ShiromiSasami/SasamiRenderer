#include "Component/SkinnedMeshComponent.h"

#include <atomic>
#include <cstdint>
#include <utility>

#include "ApplicationResourcePaths.h"
#include "Loader/AssetLoader.h"
#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    namespace
    {
        static std::atomic<uint64_t> g_cpuSkinnedTextureIdCounter{ 1000000 };

        std::shared_ptr<const CpuTextureRgba8> LoadCpuTextureFromPath(const std::string& path)
        {
            if (path.empty()) {
                return nullptr;
            }

            UINT textureWidth = 0;
            UINT textureHeight = 0;
            std::vector<uint8_t> pixels;
            const std::wstring resolvedPath = ApplicationResourcePaths::ResolveAssetPathWide(path);
            if (!AssetLoader::LoadRgba8ViaWIC(resolvedPath, pixels, textureWidth, textureHeight)) {
                return nullptr;
            }

            auto textureData = std::make_shared<CpuTextureRgba8>();
            textureData->id = g_cpuSkinnedTextureIdCounter.fetch_add(1, std::memory_order_relaxed);
            textureData->pixels = std::move(pixels);
            textureData->width = textureWidth;
            textureData->height = textureHeight;
            return textureData;
        }

        bool IsTransparentMaterial(const SurfaceMaterial& material)
        {
            static constexpr float kOpaqueAlphaThreshold = 0.999f;
            static constexpr float kTransparentTransmissionThreshold = 0.01f;
            return material.baseColor[3] < kOpaqueAlphaThreshold ||
                   material.transmission > kTransparentTransmissionThreshold;
        }
    }

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

        m_meshes.reserve(modelData.meshes.size());
        for (size_t i = 0; i < modelData.meshes.size(); ++i) {
            SkinnedMeshSource source;
            source.mesh = std::move(modelData.meshes[i]);
            if (i < modelData.albedoTexturePaths.size()) {
                source.albedoTexture = LoadCpuTextureFromPath(modelData.albedoTexturePaths[i]);
            }
            if (i < modelData.occlusionTexturePaths.size()) {
                source.occlusionTexture = LoadCpuTextureFromPath(modelData.occlusionTexturePaths[i]);
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
            proxy.animController = &m_animationController;
            proxy.albedoTexture = source.albedoTexture;
            proxy.occlusionTexture = source.occlusionTexture;
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

    void SkinnedMeshComponent::Clear()
    {
        m_meshes.clear();
        m_skeleton.reset();
        m_animationController = AnimationController{};
        m_debugAssetPath.clear();
        for (int i = 0; i < 16; ++i) {
            m_model[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
    }
}
