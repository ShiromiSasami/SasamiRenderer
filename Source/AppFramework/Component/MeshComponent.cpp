#include "Component/MeshComponent.h"

#include "ApplicationResourcePaths.h"
#include "Component/CpuTextureLoader.h"
#include "Foundation/Math/MathUtil.h"
#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;

    bool MeshComponent::LoadStaticMeshSources(const std::string& assetPath, ModelFormat format,
                                              float uniformScale, std::vector<StaticMeshSource>& outMeshes)
    {
        const std::string fullPath = ApplicationResourcePaths::ResolveAssetPathString(assetPath);

        StaticModelFormat loaderFormat = StaticModelFormat::Obj;
        switch (format) {
        case ModelFormat::Obj:
            loaderFormat = StaticModelFormat::Obj;
            break;
        case ModelFormat::Gltf:
            loaderFormat = StaticModelFormat::Gltf;
            break;
        case ModelFormat::Fbx:
            loaderFormat = StaticModelFormat::Fbx;
            break;
        default:
            return false;
        }

        std::vector<LoadedStaticMesh> loadedMeshes;
        if (!LoadStaticModel(fullPath, loaderFormat, uniformScale, loadedMeshes)) {
            return false;
        }

        outMeshes.reserve(outMeshes.size() + loadedMeshes.size());
        for (auto& loaded : loadedMeshes) {
            StaticMeshSource src;
            src.mesh = std::move(loaded.mesh);
            src.albedoTexture = LoadCpuTextureFromPath(loaded.texturePath);
            src.occlusionTexture = LoadCpuTextureFromPath(loaded.occlusionTexturePath);
            src.normalTexture = LoadCpuTextureFromPath(loaded.normalTexturePath);
            src.material = loaded.material;
            if (!loaded.metallicRoughnessTexturePath.empty()) {
                src.usesMetallicRoughnessTexture = true;
                if (loaded.metallicRoughnessTexturePath == loaded.occlusionTexturePath && src.occlusionTexture) {
                    // glTF commonly packs occlusion(R), roughness(G), metallic(B) in one texture.
                } else {
                    src.occlusionTexture = LoadCpuTextureFromPath(loaded.metallicRoughnessTexturePath);
                    // The current raster material root layout has one material texture slot.
                    // Prefer roughness/metallic and disable AO if glTF uses separate images.
                    src.material.occlusionStrength = 0.0f;
                }
            }
            for (int i = 0; i < 16; ++i) {
                src.localTransform[i] = loaded.localTransform[i];
            }
            outMeshes.push_back(std::move(src));
        }

        return !loadedMeshes.empty();
    }

    void MeshComponent::AdoptLoadedMeshes(std::vector<StaticMeshSource>&& meshes, const std::string& assetPath,
                                          ModelFormat format, float uniformScale)
    {
        Clear();
        m_staticMeshes = std::move(meshes);
        m_debugAssetPath = assetPath;
        m_loadedFormat = format;
        m_loadedUniformScale = uniformScale;
        m_loadState = MeshLoadState::Ready;
    }

    bool MeshComponent::LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale)
    {
        std::vector<StaticMeshSource> loadedMeshes;
        if (!LoadStaticMeshSources(assetPath, format, uniformScale, loadedMeshes)) {
            m_loadState = MeshLoadState::Failed;
            return false;
        }

        AdoptLoadedMeshes(std::move(loadedMeshes), assetPath, format, uniformScale);
        return true;
    }

    void MeshComponent::AddStaticMesh(Mesh mesh,
                                      const std::string& albedoTexturePath,
                                      const std::string& occlusionTexturePath,
                                      const std::string& normalTexturePath)
    {
        AddStaticMesh(std::move(mesh), SurfaceMaterial{}, albedoTexturePath, occlusionTexturePath, normalTexturePath);
    }

    void MeshComponent::AddStaticMesh(Mesh mesh,
                                      const SurfaceMaterial& material,
                                      const std::string& albedoTexturePath,
                                      const std::string& occlusionTexturePath,
                                      const std::string& normalTexturePath)
    {
        StaticMeshSource src;
        src.mesh = std::move(mesh);
        src.albedoTexture = LoadCpuTextureFromPath(albedoTexturePath);
        src.occlusionTexture = LoadCpuTextureFromPath(occlusionTexturePath);
        src.normalTexture = LoadCpuTextureFromPath(normalTexturePath);
        src.material = material;
        m_staticMeshes.push_back(std::move(src));
        m_loadState = MeshLoadState::Ready;
    }

    std::vector<RenderProxy> MeshComponent::BuildRenderProxies() const
    {

        std::vector<RenderProxy> proxies;
        proxies.reserve(m_staticMeshes.size());

        for (const auto& src : m_staticMeshes) {
            RenderProxy proxy;
            proxy.mesh = src.mesh;
            proxy.albedoTexture = src.albedoTexture;
            proxy.occlusionTexture = src.occlusionTexture;
            proxy.normalTexture = src.normalTexture;
            proxy.usesMetallicRoughnessTexture = src.usesMetallicRoughnessTexture;
            proxy.material = src.material;
            proxy.transparent = IsTransparentMaterial(src.material);
            proxy.debugLabel = m_debugAssetPath;
            // Final model matrix for draw = local mesh transform * component transform.
            Mul4x4(src.localTransform, m_model, proxy.model);
            proxies.push_back(std::move(proxy));
        }

        return proxies;
    }

    bool MeshComponent::SetMaterial(size_t meshIndex, const SurfaceMaterial& material)
    {
        if (meshIndex >= m_staticMeshes.size()) {
            return false;
        }

        m_staticMeshes[meshIndex].material = material;
        return true;
    }

    const SurfaceMaterial* MeshComponent::GetMaterial(size_t meshIndex) const
    {
        if (meshIndex >= m_staticMeshes.size()) {
            return nullptr;
        }

        return &m_staticMeshes[meshIndex].material;
    }

    void MeshComponent::Clear()
    {
        m_staticMeshes.clear();
        m_debugAssetPath.clear();
        Math::Identity4x4(m_model);
        m_loadState = MeshLoadState::Empty;
    }

    void MeshComponent::SetTranslation(float x, float y, float z)
    {
        // Row-major affine translation slots (row-vector convention).
        m_model[12] = x;
        m_model[13] = y;
        m_model[14] = z;
    }
}
