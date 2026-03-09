#include "Object/MeshComponent.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <windows.h>

#include "Foundation/Math/MathUtil.h"
#include "Loader/AssetLoader.h"
#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;

    namespace
    {
        static std::atomic<uint64_t> g_cpuTextureIdCounter{ 1 };

        static std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        static std::filesystem::path FindProjectRootWithAssets(const std::filesystem::path& startDir)
        {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
            if (ec) {
                dir = startDir;
            }

            for (;;) {
                const std::filesystem::path assetsDir = dir / L"Assets";
                if (std::filesystem::exists(assetsDir, ec) &&
                    std::filesystem::is_directory(assetsDir, ec)) {
                    return dir;
                }

                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }

            return {};
        }

        static std::filesystem::path ResolveAssetPath(const std::string& relative)
        {
            const std::filesystem::path relativePath(relative);
            if (relativePath.is_absolute()) {
                return relativePath;
            }

            const std::filesystem::path projectRoot = FindProjectRootWithAssets(GetExecutableDir());
            if (!projectRoot.empty()) {
                return projectRoot / L"Assets" / relativePath;
            }

            return std::filesystem::current_path() / L"Assets" / relativePath;
        }

        static std::shared_ptr<const CpuTextureRgba8> LoadCpuTextureFromPath(const std::string& path)
        {
            if (path.empty()) {
                return nullptr;
            }

            UINT textureWidth = 0;
            UINT textureHeight = 0;
            std::vector<uint8_t> pixels;
            const std::filesystem::path resolvedPath = ResolveAssetPath(path);
            if (!AssetLoader::LoadRgba8ViaWIC(resolvedPath.wstring(), pixels, textureWidth, textureHeight)) {
                return nullptr;
            }

            auto textureData = std::make_shared<CpuTextureRgba8>();
            textureData->id = g_cpuTextureIdCounter.fetch_add(1, std::memory_order_relaxed);
            textureData->pixels = std::move(pixels);
            textureData->width = textureWidth;
            textureData->height = textureHeight;
            return textureData;
        }

    }

    bool MeshComponent::LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale)
    {
        Clear();
        const std::string fullPath = ResolveAssetPath(assetPath).string();

        StaticModelFormat loaderFormat = StaticModelFormat::Obj;
        switch (format) {
        case ModelFormat::Obj:
            loaderFormat = StaticModelFormat::Obj;
            break;
        case ModelFormat::Gltf:
            loaderFormat = StaticModelFormat::Gltf;
            break;
        default:
            return false;
        }

        std::vector<LoadedStaticMesh> loadedMeshes;
        if (!LoadStaticModel(fullPath, loaderFormat, uniformScale, loadedMeshes)) {
            return false;
        }

        m_staticMeshes.reserve(loadedMeshes.size());
        for (auto& loaded : loadedMeshes) {
            StaticMeshSource src;
            src.mesh = std::move(loaded.mesh);
            src.albedoTexture = LoadCpuTextureFromPath(loaded.texturePath);
            src.occlusionTexture = LoadCpuTextureFromPath(loaded.occlusionTexturePath);
            for (int i = 0; i < 16; ++i) {
                src.localTransform[i] = loaded.localTransform[i];
            }
            m_staticMeshes.push_back(std::move(src));
        }
        return !m_staticMeshes.empty();
    }

    void MeshComponent::AddStaticMesh(Mesh mesh,
                                      const std::string& albedoTexturePath,
                                      const std::string& occlusionTexturePath)
    {
        StaticMeshSource src;
        src.mesh = std::move(mesh);
        src.albedoTexture = LoadCpuTextureFromPath(albedoTexturePath);
        src.occlusionTexture = LoadCpuTextureFromPath(occlusionTexturePath);
        m_staticMeshes.push_back(std::move(src));
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
            // Final model matrix for draw = local mesh transform * component transform.
            Mul4x4(src.localTransform, m_model, proxy.model);
            proxies.push_back(std::move(proxy));
        }

        return proxies;
    }

    void MeshComponent::Clear()
    {
        m_staticMeshes.clear();
        for (int i = 0; i < 16; ++i) {
            m_model[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
    }

    void MeshComponent::SetTranslation(float x, float y, float z)
    {
        // Row-major affine translation slots (row-vector convention).
        m_model[12] = x;
        m_model[13] = y;
        m_model[14] = z;
    }
}
