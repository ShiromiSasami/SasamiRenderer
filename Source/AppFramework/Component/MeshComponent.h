#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include <memory>

#include "AppFramework/Component/IComponent.h"
#include "Foundation/Math/Vector3.h"
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
            Fbx,
            GltfStatic = Gltf, // Backward compatibility alias
        };

        enum class MeshLoadState
        {
            Empty,   // no meshes and no load requested
            Loading, // async load in flight (see AsyncAssetLoadService)
            Ready,   // meshes present (loaded or procedural)
            Failed
        };

        struct StaticMeshSource
        {
            Mesh mesh;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            std::shared_ptr<const CpuTextureRgba8> normalTexture;
            bool usesMetallicRoughnessTexture = false;
            SurfaceMaterial material;
            float localTransform[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        bool LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale = 1.0f);

        // CPU-side load only (glTF/OBJ parse + texture decode via the path cache).
        // No GPU or component access -- safe to call from a JobSystem worker.
        static bool LoadStaticMeshSources(const std::string& assetPath, ModelFormat format,
                                          float uniformScale, std::vector<StaticMeshSource>& outMeshes);
        // Main thread: replaces current meshes with worker-loaded data and sets Ready.
        void AdoptLoadedMeshes(std::vector<StaticMeshSource>&& meshes, const std::string& assetPath,
                               ModelFormat format, float uniformScale);
        void MarkLoadPending() { m_loadState = MeshLoadState::Loading; }
        void MarkLoadFailed()  { m_loadState = MeshLoadState::Failed; }
        MeshLoadState GetLoadState() const { return m_loadState; }
        void AddStaticMesh(Mesh mesh,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "",
                           const std::string& normalTexturePath = "");
        void AddStaticMesh(Mesh mesh,
                           const SurfaceMaterial& material,
                           const std::string& albedoTexturePath = "",
                           const std::string& occlusionTexturePath = "",
                           const std::string& normalTexturePath = "");
        std::vector<RenderProxy> BuildRenderProxies() const;
        bool SetMaterial(size_t meshIndex, const SurfaceMaterial& material);
        const SurfaceMaterial* GetMaterial(size_t meshIndex) const;

        void Clear();
        void SetTranslation(float x, float y, float z);

        const std::string& GetAssetPath() const { return m_debugAssetPath; }
        ModelFormat GetLoadedFormat() const      { return m_loadedFormat; }
        float GetLoadedUniformScale() const      { return m_loadedUniformScale; }
        Vector3 GetTranslation() const           { return { m_model[12], m_model[13], m_model[14] }; }

    private:
        std::vector<StaticMeshSource> m_staticMeshes;
        MeshLoadState m_loadState = MeshLoadState::Empty;
        std::string m_debugAssetPath;
        ModelFormat m_loadedFormat = ModelFormat::Gltf;
        float m_loadedUniformScale = 1.0f;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
