#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

#include "AppFramework/Component/IComponent.h"
#include "Foundation/Math/Vector3.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::App
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
            std::shared_ptr<const Mesh> mesh;
            std::shared_ptr<const CpuTextureRgba8> albedoTexture;
            std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
            std::shared_ptr<const CpuTextureRgba8> normalTexture;
            std::shared_ptr<const CpuTextureRgba8> emissiveTexture;
            bool usesMetallicRoughnessTexture = false;
            SurfaceMaterial material;
            bool dynamicVertices = false; // RenderProxy::dynamicVertices
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
        // Marks a mesh whose vertices the caller rewrites through Renderer::UpdateStaticMeshDynamicVertices.
        // Takes effect on the next render-object rebuild.
        bool SetDynamicVertices(size_t meshIndex, bool dynamic);

        void Clear();
        void SetTranslation(float x, float y, float z);

        // Baked placements ([instance] in a .scene file). Empty means the component draws once
        // with m_model; otherwise every source is emitted once per entry, all proxies of one source
        // sharing its Mesh pointer. Each entry sits between the mesh-local and component transform.
        using InstanceTransform = std::array<float, 16>;
        void SetInstanceTransforms(std::vector<InstanceTransform> transforms) { m_instanceTransforms = std::move(transforms); }
        const std::vector<InstanceTransform>& GetInstanceTransforms() const   { return m_instanceTransforms; }

        const std::string& GetAssetPath() const { return m_debugAssetPath; }
        ModelFormat GetLoadedFormat() const      { return m_loadedFormat; }
        float GetLoadedUniformScale() const      { return m_loadedUniformScale; }
        Vector3 GetTranslation() const           { return { m_model[12], m_model[13], m_model[14] }; }
        // Row-major, row-vector component transform (translation in [12..14]).
        void SetModelMatrix(const float matrix[16]) { for (int i = 0; i < 16; ++i) m_model[i] = matrix[i]; }
        const float* GetModelMatrix() const      { return m_model; }
        // Axis-aligned bounds of every vertex after its local mesh transform and instance placement,
        // i.e. in the space the component transform (GetModelMatrix) maps to world. False when empty.
        bool ComputeLocalBounds(float outMin[3], float outMax[3]) const;

    private:
        std::vector<StaticMeshSource> m_staticMeshes;
        MeshLoadState m_loadState = MeshLoadState::Empty;
        std::string m_debugAssetPath;
        ModelFormat m_loadedFormat = ModelFormat::Gltf;
        float m_loadedUniformScale = 1.0f;
        std::vector<InstanceTransform> m_instanceTransforms;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
namespace SasamiEngine { using namespace App; }
