#include "Object/MeshComponent.h"

#include <filesystem>
#include <windows.h>

#include "Foundation/Math/MathUtil.h"
#include "Loader/ModelLoader.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;

    namespace
    {
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

        static std::filesystem::path ResolveAssetPath(const std::wstring& relative)
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

    }

    bool MeshComponent::LoadModel(const std::wstring& assetPath, ModelFormat format, float uniformScale)
    {
        Clear();
        const std::wstring fullPath = ResolveAssetPath(assetPath).wstring();

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
            src.texturePath = std::move(loaded.texturePath);
            for (int i = 0; i < 16; ++i) {
                src.localTransform[i] = loaded.localTransform[i];
            }
            m_staticMeshes.push_back(std::move(src));
        }
        return !m_staticMeshes.empty();
    }

    void MeshComponent::AddStaticMesh(Mesh mesh, const std::wstring& texturePath)
    {
        StaticMeshSource src;
        src.mesh = std::move(mesh);
        src.texturePath = texturePath;
        m_staticMeshes.push_back(std::move(src));
    }

    std::vector<RenderProxy> MeshComponent::BuildRenderProxies() const
    {
        std::vector<RenderProxy> proxies;
        proxies.reserve(m_staticMeshes.size());

        for (const auto& src : m_staticMeshes) {
            RenderProxy proxy;
            proxy.mesh = src.mesh;
            proxy.texturePath = src.texturePath;
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
        m_model[12] = x;
        m_model[13] = y;
        m_model[14] = z;
    }
}
